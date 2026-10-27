#include "netconfig.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mdns.h>
#include <lwip/ip4_addr.h>
#include <lwip/sockets.h>
#include "captive_dns.h"
#include "setup_policy.h"

#include "health.h"

namespace ledcast {
namespace {
constexpr char kTag[] = "ledcast-net";
constexpr uint16_t kScanLimit = 32;
// Столько ответ на запрос, изменивший Wi-Fi, получает, чтобы уйти по нынешнему
// соединению, прежде чем строка его разорвёт.
constexpr int64_t kReplyGraceUs = 1000000;
struct State {
    bool started = false;
    WifiConfig config{};
    esp_netif_t* ap = nullptr;
    esp_netif_t* sta = nullptr;
    SemaphoreHandle_t lock = nullptr;
    std::atomic<bool> connected{false};
    std::atomic<bool> connecting{false};
    std::atomic<bool> ap_enabled{false};
    // Автономный запуск: точка доступа без попыток уйти в сохранённые сети.
    std::atomic<bool> force_ap{false};
    std::atomic<uint8_t> profile{0};
    std::atomic<bool> rebind_scheduled{false};
    NetInterfaceChangedHandler interface_changed = nullptr;
    char ap_ssid[33]{};
    char hostname[32]{};
    std::atomic<bool> start_requested{false};
    std::atomic<bool> disconnected{false};
    std::atomic<bool> got_ip{false};
    std::atomic<bool> scanning{false};
    int64_t retry_at = 0;
    int64_t connect_deadline = 0;
    int64_t restart_at = 0;
    unsigned failures = 0;
    bool background_connect = false;
    std::atomic<uint16_t> disconnect_reason{0};
} s;

std::atomic<bool> dns_running{false};
SemaphoreHandle_t dns_done = nullptr;

void dns_task(void*) {
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = inet_addr("192.168.4.1");
    timeval timeout{0, 250000};
    if (fd >= 0 && setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0) {
        uint8_t packet[528];
        constexpr uint8_t address[] = {192, 168, 4, 1};
        while (dns_running.load()) {
            sockaddr_in peer{};
            socklen_t size = sizeof(peer);
            const int n = recvfrom(fd, packet, sizeof(packet), 0,
                                   reinterpret_cast<sockaddr*>(&peer), &size);
            // Never reflect replies outside the setup subnet.
            if (n <= 0 || (ntohl(peer.sin_addr.s_addr) & 0xffffff00u) != 0xc0a80400u) continue;
            const size_t reply = captive::dns_reply(packet, n, sizeof(packet), address);
            if (reply && dns_running.load()) sendto(fd, packet, reply, 0,
                                                    reinterpret_cast<sockaddr*>(&peer), size);
        }
    } else ESP_LOGW(kTag, "setup DNS could not bind");
    if (fd >= 0) close(fd);
    dns_running.store(false);
    xSemaphoreGive(dns_done);
    vTaskDelete(nullptr);
}

// Wi-Fi events are serialized. Stop waits for bounded recv timeout, allowing
// socket and task memory to be released before an AP restart creates another.
bool dns_started = false;
void stop_dns() {
    if (!dns_started) return;
    dns_running.store(false);
    xSemaphoreTake(dns_done, portMAX_DELAY);
    dns_started = false;
    vSemaphoreDelete(dns_done);
    dns_done = nullptr;
}
void start_dns() {
    stop_dns();
    if (!dns_done) dns_done = xSemaphoreCreateBinary();
    if (!dns_done) { ESP_LOGW(kTag, "setup DNS semaphore allocation failed"); return; }
    dns_running.store(true);
    dns_started = xTaskCreate(dns_task, "setup_dns", 2560, nullptr, 2, nullptr) == pdPASS;
    if (!dns_started) {
        dns_running.store(false);
        vSemaphoreDelete(dns_done); dns_done = nullptr;
        ESP_LOGW(kTag, "setup DNS task allocation failed");
    }
}

// Имя строки для людей: заданное, иначе сетевое (ledcast-a4dc).
const char* display_name() { return s.config.hostname[0] ? s.config.hostname : s.hostname; }

esp_err_t start_mdns() {
    const esp_err_t error = mdns_init();
    if (error != ESP_OK) return error;
    mdns_hostname_set(s.hostname);
    mdns_instance_name_set(display_name());
    return mdns_service_add(display_name(), "_ledcast", "_tcp", 80, nullptr, 0);
}

void rebind_task(void*) {
    // esp_wifi_set_mode() tears down the removed interface asynchronously.
    // Recreate network-bound services only after lwIP has processed that
    // transition; otherwise their sockets remain attached to the AP netif.
    vTaskDelay(pdMS_TO_TICKS(150));
    mdns_free();
    const esp_err_t mdns_error = start_mdns();
    if (mdns_error != ESP_OK) ESP_LOGW(kTag, "mDNS rebind failed: %s", esp_err_to_name(mdns_error));
    if (s.interface_changed != nullptr) s.interface_changed();
    s.rebind_scheduled.store(false);
    vTaskDelete(nullptr);
}

void schedule_rebind() {
    if (s.interface_changed == nullptr || s.rebind_scheduled.exchange(true)) return;
    if (xTaskCreate(rebind_task, "net_rebind", 4096, nullptr, 3, nullptr) != pdPASS) {
        s.rebind_scheduled.store(false);
        ESP_LOGE(kTag, "failed to create network rebind task");
    }
}

void lock() { if (s.lock) xSemaphoreTake(s.lock, portMAX_DELAY); }
void unlock() { if (s.lock) xSemaphoreGive(s.lock); }

void identities() {
    // Тот же MAC, что в id строки: имя в сети и точка доступа совпадают с ним.
    uint8_t mac[6]{}; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s.ap_ssid, sizeof(s.ap_ssid), "LEDCAST-%02X%02X", mac[4], mac[5]);
    snprintf(s.hostname, sizeof(s.hostname), "ledcast-%02x%02x", mac[4], mac[5]);
}

esp_err_t select_profile(uint8_t index) {
    wifi_config_t sta{};
    if (index >= s.config.profile_count) {
        s.connecting.store(false); s.profile.store(0); return esp_wifi_set_config(WIFI_IF_STA, &sta);
    }
    const auto& profile = s.config.profiles[index];
    snprintf(reinterpret_cast<char*>(sta.sta.ssid), sizeof(sta.sta.ssid), "%s", profile.ssid);
    snprintf(reinterpret_cast<char*>(sta.sta.password), sizeof(sta.sta.password), "%s", profile.password);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    s.profile.store(index); s.connecting.store(true);
    return esp_wifi_set_config(WIFI_IF_STA, &sta);
}

void event_handler(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        start_dns();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        stop_dns();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s.start_requested.store(true);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s.connected.store(false);
        s.disconnected.store(true);
        const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
        s.disconnect_reason.store(event ? event->reason : 0);
        ESP_LOGW(kTag, "STA disconnected, reason=%u", event ? event->reason : 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(data);
        if (event != nullptr) {
            ESP_LOGI(kTag, "STA got IP " IPSTR ", gateway " IPSTR,
                     IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        }
        s.got_ip.store(true);
    }
}

esp_err_t setup_ap() {
    if (s.ap_enabled.load()) return ESP_OK;
    const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        s.ap_enabled.store(true);
        ESP_LOGI(kTag, "setup AP %s at 192.168.4.1", s.ap_ssid);
        schedule_rebind();
    }
    return err;
}

void begin_connection(uint8_t profile, bool background = false) {
    s.background_connect = background;
    s.disconnected.store(false);
    s.retry_at = 0;
    esp_err_t err = select_profile(profile);
    if (err == ESP_OK) err = esp_wifi_connect();
    s.connecting.store(err == ESP_OK);
    s.connect_deadline = esp_timer_get_time() + 15000000;
    if (err != ESP_OK) s.disconnected.store(true);
    health_led_set(LedPattern::StaConnecting);
}

// The sole reconnect scheduler. Event callbacks never initiate scans/connections
// or mutate profiles, so intentional disconnects cannot start a reconnect loop.
void radio_task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        lock();
        const int64_t now = esp_timer_get_time();
        wifi_sta_list_t clients{};
        if (s.ap_enabled.load()) esp_wifi_ap_get_sta_list(&clients);
        if (s.got_ip.exchange(false)) {
            wifi_ap_record_t ap{};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                s.connected.store(true); s.connecting.store(false);
                s.disconnected.store(false); s.retry_at = 0; s.failures = 0;
                health_led_set(LedPattern::Running);
            }
        }
        if (s.disconnected.exchange(false) && !s.connected.load()) {
            s.connecting.store(false);
            setup_ap();
            s.retry_at = now + netconfig::retry_delay_us(++s.failures);
            health_led_set(LedPattern::ApWaiting);
        }
        if (s.start_requested.exchange(false) && s.config.profile_count && !s.force_ap.load())
            begin_connection(0);
        if (s.restart_at != 0 && now >= s.restart_at) {
            // Новое подключение — на следующем такте, после события разрыва.
            // Wi-Fi правили из интерфейса — автономный запуск на этом кончается.
            s.restart_at = 0; s.force_ap.store(false);
            s.connected.store(false); s.connecting.store(false);
            s.retry_at = 0; s.failures = 0; esp_wifi_disconnect();
            s.start_requested.store(true);
        }
        if (s.connecting.load() && (now >= s.connect_deadline ||
                                  (s.background_connect && clients.num > 0))) {
            esp_wifi_disconnect();
            s.connecting.store(false);
            setup_ap();
            s.retry_at = now + netconfig::retry_delay_us(++s.failures);
        }
        if (s.connected.load() && s.ap_enabled.load() && clients.num == 0) {
            if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
                // Смена режима не должна вернуть сон радио: он копит кадры до маяка.
                esp_wifi_set_ps(WIFI_PS_NONE);
                s.ap_enabled.store(false); schedule_rebind();
            }
        }
        if (s.config.profile_count && !s.force_ap.load() && netconfig::may_retry(s.connected.load(),
                s.connecting.load(), s.scanning.load(), clients.num, now, s.retry_at))
            begin_connection((s.profile.load() + 1u) % s.config.profile_count, true);
        unlock();
    }
}

esp_err_t set_ap_ip() {
    esp_netif_ip_info_t info{};
    // AP не должен попадать в ту же /24-сеть, что и STA: иначе ESP
    // отвечает клиентам локальной Wi-Fi сети через неверный интерфейс.
    IP4_ADDR(&info.ip, 192,168,4,1); IP4_ADDR(&info.gw,192,168,4,1); IP4_ADDR(&info.netmask,255,255,255,0);
    esp_netif_dhcps_stop(s.ap); esp_err_t error = esp_netif_set_ip_info(s.ap,&info);
    if (error != ESP_OK) return error;
    // ESP-IDF softap_sta example: advertise this AP as DHCP's DNS server.
    uint8_t offer_dns = 0x02;
    error = esp_netif_dhcps_option(s.ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                  &offer_dns, sizeof(offer_dns));
    esp_netif_dns_info_t dns{};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = info.ip;
    if (error == ESP_OK) error = esp_netif_set_dns_info(s.ap, ESP_NETIF_DNS_MAIN, &dns);
    if (error == ESP_OK) error = esp_netif_dhcps_start(s.ap);
    return error;
}

esp_err_t apply(const WifiConfig& config) {
    s.config = config;
    // A configured node is a LAN appliance, not an access point. Starting in
    // APSTA and removing the AP after DHCP changes the lwIP interface under
    // live sockets and, on some routers, leaves the STA unreachable until the
    // next reconnect. Boot directly in STA mode whenever credentials exist;
    // the AP remains the recovery path when there is no saved network or the
    // connection is later lost.
    const bool needs_setup_ap = config.profile_count == 0 || s.ap_enabled.load() || s.force_ap.load();
    // Configure the AP even on a normal LAN boot. Otherwise a later fallback
    // advertises the driver's factory ESP_* SSID and default security settings.
    esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (error != ESP_OK) return error;
    s.ap_enabled.store(needs_setup_ap);

    wifi_config_t ap{};
    ap.ap.ssid_len = static_cast<uint8_t>(strnlen(s.ap_ssid, sizeof(ap.ap.ssid)));
    memcpy(ap.ap.ssid, s.ap_ssid, ap.ap.ssid_len);
    const size_t password_len = strnlen(config.ap_password, sizeof(ap.ap.password));
    memcpy(ap.ap.password, config.ap_password, password_len);
    ap.ap.channel=config.ap_channel; ap.ap.max_connection=8; ap.ap.authmode=WIFI_AUTH_WPA2_PSK;
    error=esp_wifi_set_config(WIFI_IF_AP,&ap); if(error!=ESP_OK)return error;
    if (!needs_setup_ap && (error=esp_wifi_set_mode(WIFI_MODE_STA))!=ESP_OK) return error;
    esp_wifi_set_ps(WIFI_PS_NONE);
    s.connecting.store(false);
    return ESP_OK;
}

void copy_ip(char* out,size_t length,esp_netif_t* netif) { esp_netif_ip_info_t info{}; if(netif&&esp_netif_get_ip_info(netif,&info)==ESP_OK) snprintf(out,length,IPSTR,IP2STR(&info.ip)); }
bool known(const char* ssid) { for(uint8_t i=0;i<s.config.profile_count;++i) if(strcmp(ssid,s.config.profiles[i].ssid)==0)return true; return false; }
}  // namespace

esp_err_t net_start(const WifiConfig& config) {
    if(s.started)return net_reconfigure(config);
    s.lock=xSemaphoreCreateMutex(); if(!s.lock)return ESP_ERR_NO_MEM; identities();
    esp_err_t error=esp_netif_init(); if(error!=ESP_OK)return error;
    error=esp_event_loop_create_default(); if(error!=ESP_OK&&error!=ESP_ERR_INVALID_STATE)return error;
    s.ap=esp_netif_create_default_wifi_ap(); s.sta=esp_netif_create_default_wifi_sta();
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT(); if((error=esp_wifi_init(&init))!=ESP_OK)return error;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event_handler,nullptr);
    esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,event_handler,nullptr);
    if((error=apply(config))!=ESP_OK)return error;
    if((error=set_ap_ip())!=ESP_OK)return error;
    esp_netif_set_hostname(s.ap,s.hostname); esp_netif_set_hostname(s.sta,s.hostname);
    if((error=esp_wifi_start())!=ESP_OK)return error;
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Highest supported API limit. PHY calibration and country limits still
    // apply; 80..84 map to 20 dBm on this ESP32-S3, not 21 dBm.
    error=esp_wifi_set_max_tx_power(84);
    if(error!=ESP_OK)return error;
    if (xTaskCreate(radio_task, "wifi_control", 3072, nullptr, 2, nullptr) != pdPASS)
        return ESP_ERR_NO_MEM;
    const esp_err_t mdns_error = start_mdns();
    if (mdns_error != ESP_OK) ESP_LOGW(kTag, "mDNS start failed: %s", esp_err_to_name(mdns_error));
    s.started=true;
    if (s.ap_enabled.load()) ESP_LOGI(kTag,"setup AP %s at 192.168.4.1",s.ap_ssid);
    else ESP_LOGI(kTag,"LAN mode, %u saved networks",config.profile_count);
    return ESP_OK;
}

const char* net_hostname() { return s.hostname; }

void net_force_setup_ap() { s.force_ap.store(true); }

void net_set_interface_changed_handler(NetInterfaceChangedHandler handler) {
    s.interface_changed = handler;
}

esp_err_t net_reconfigure(const WifiConfig& config) {
    if(!s.started)return net_start(config);
    lock();
    // Do not stop the radio/DHCP server underneath the phone submitting a
    // profile. The STA link drops and reconnects only after this reply left.
    const bool ap_changed = s.config.ap_channel != config.ap_channel ||
        strcmp(s.config.ap_password,config.ap_password) != 0;
    // Имя строки — только для людей и mDNS: ради него связь с роутером не рвётся.
    const bool sta_changed = ap_changed || s.config.profile_count != config.profile_count ||
        memcmp(s.config.profiles,config.profiles,sizeof(config.profiles)) != 0;
    esp_err_t error=ESP_OK;
    if(ap_changed) error=apply(config);
    else {
        s.config=config;
        if(!config.profile_count) error=setup_ap();
    }
    // Имя строки поменяли — анонс в сети следом.
    mdns_instance_name_set(display_name());
    mdns_service_instance_name_set("_ledcast","_tcp",display_name());
    if(error==ESP_OK && sta_changed) s.restart_at=esp_timer_get_time()+kReplyGraceUs;
    unlock(); return error;
}

NetStatus net_get_status() {
    NetStatus out{}; snprintf(out.ap_ssid,sizeof(out.ap_ssid),"%s",s.ap_ssid);
    out.disconnect_reason=s.disconnect_reason.load();
    esp_wifi_get_max_tx_power(&out.tx_power_quarter_dbm);
    wifi_ps_type_t ps=WIFI_PS_NONE; esp_wifi_get_ps(&ps); out.power_save=static_cast<uint8_t>(ps);
    wifi_second_chan_t secondary{}; esp_wifi_get_channel(&out.channel,&secondary);
    if (s.ap_enabled.load()) {
        copy_ip(out.ap_ip,sizeof(out.ap_ip),s.ap);
        wifi_sta_list_t clients{}; if(esp_wifi_ap_get_sta_list(&clients)==ESP_OK)out.sta_clients=clients.num;
    }
    if(s.connected.load()){ out.state=s.ap_enabled.load()?NetState::ApSta:NetState::StaConnected; copy_ip(out.ip,sizeof(out.ip),s.sta); wifi_ap_record_t ap{}; if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){out.rssi=ap.rssi;snprintf(out.ssid,sizeof(out.ssid),"%s",reinterpret_cast<char*>(ap.ssid));}}
    else if(s.connecting.load()){
        out.state=NetState::StaConnecting;
        const auto snapshot=config_get(); const uint8_t profile=s.profile.load();
        if(profile<snapshot->wifi.profile_count)
            snprintf(out.ssid,sizeof(out.ssid),"%s",snapshot->wifi.profiles[profile].ssid);
    }
    else out.state=s.started&&s.ap_enabled.load()?NetState::ApOnly:NetState::Down;
    return out;
}

int net_scan(ScanResult* out,size_t max_results) {
    if(!out||!max_results||!s.started)return -1;
    lock(); s.scanning.store(true);
    // Cancel an unfinished STA attempt before scanning. A connected LAN stays
    // connected; only the unavailable-network setup path is interrupted.
    if (!s.connected.load()) {
        s.start_requested.store(false);
        esp_wifi_disconnect();
        s.connecting.store(false);
        s.retry_at=esp_timer_get_time()+60000000;
    }
    struct ScanGuard { ~ScanGuard() { s.scanning.store(false); } } guard;
    wifi_scan_config_t config{}; config.show_hidden=false;
    config.scan_time.active.min=30; config.scan_time.active.max=120;
    const esp_err_t scan_error=esp_wifi_scan_start(&config,true);
    if(scan_error!=ESP_OK){ESP_LOGW(kTag,"scan failed: %s",esp_err_to_name(scan_error));unlock();return -1;} uint16_t count=kScanLimit;
    auto* records=static_cast<wifi_ap_record_t*>(calloc(count,sizeof(wifi_ap_record_t))); if(!records){esp_wifi_clear_ap_list();unlock();return -1;}
    if(esp_wifi_scan_get_ap_records(&count,records)!=ESP_OK){free(records);esp_wifi_clear_ap_list();unlock();return -1;}
    size_t written=0; for(uint16_t i=0;i<count&&written<max_results;++i){const char* name=reinterpret_cast<const char*>(records[i].ssid);if(!*name)continue;bool duplicate=false;for(size_t j=0;j<written;++j)duplicate|=strcmp(out[j].ssid,name)==0;if(duplicate)continue;snprintf(out[written].ssid,sizeof(out[written].ssid),"%s",name);out[written].rssi=records[i].rssi;out[written].secured=records[i].authmode!=WIFI_AUTH_OPEN;out[written].known=known(name);++written;}
    free(records); unlock(); return static_cast<int>(written);
}

esp_err_t net_profile_connect(const char* ssid) {
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    const auto snapshot = config_get();
    DeviceConfig config = *snapshot;
    uint8_t found = config.wifi.profile_count;
    for (uint8_t i = 0; i < config.wifi.profile_count; ++i) {
        if (strcmp(config.wifi.profiles[i].ssid, ssid) == 0) { found = i; break; }
    }
    if (found == config.wifi.profile_count) return ESP_ERR_NOT_FOUND;
    const WifiProfile selected = config.wifi.profiles[found];
    for (uint8_t i = found; i > 0; --i) config.wifi.profiles[i] = config.wifi.profiles[i - 1];
    config.wifi.profiles[0] = selected;
    const esp_err_t error=config_set(config, snapshot);
    // Selecting the already-first profile is still a connection command, even
    // when the immutable config store correctly skips an unchanged write.
    if(error==ESP_OK && found==0) {
        wifi_ap_record_t active{};
        if(esp_wifi_sta_get_ap_info(&active)!=ESP_OK ||
           strcmp(reinterpret_cast<const char*>(active.ssid),ssid)!=0)
            return net_reconfigure(config.wifi);
    }
    return error;
}

esp_err_t net_profile_upsert(const char* ssid,const char* password) {
    if(!ssid||!*ssid||!password)return ESP_ERR_INVALID_ARG;
    const auto snapshot = config_get(); DeviceConfig config = *snapshot; WifiProfile profile{};snprintf(profile.ssid,sizeof(profile.ssid),"%s",ssid);snprintf(profile.password,sizeof(profile.password),"%s",password);
    uint8_t found=config.wifi.profile_count;for(uint8_t i=0;i<config.wifi.profile_count;++i)if(strcmp(config.wifi.profiles[i].ssid,ssid)==0){found=i;break;}
    const uint8_t last=found<config.wifi.profile_count?found:(config.wifi.profile_count<kWifiProfileLimit?config.wifi.profile_count++:kWifiProfileLimit-1);
    for(uint8_t i=last;i>0;--i)config.wifi.profiles[i]=config.wifi.profiles[i-1];
    config.wifi.profiles[0]=profile;return config_set(config, snapshot);
}
esp_err_t net_profile_forget(const char* ssid) { if(!ssid)return ESP_ERR_INVALID_ARG;const auto snapshot = config_get(); DeviceConfig config = *snapshot;uint8_t found=config.wifi.profile_count;for(uint8_t i=0;i<config.wifi.profile_count;++i)if(strcmp(config.wifi.profiles[i].ssid,ssid)==0){found=i;break;}if(found==config.wifi.profile_count)return ESP_ERR_NOT_FOUND;for(uint8_t i=found+1;i<config.wifi.profile_count;++i)config.wifi.profiles[i-1]=config.wifi.profiles[i];--config.wifi.profile_count;config.wifi.profiles[config.wifi.profile_count]=WifiProfile{};return config_set(config, snapshot); }
esp_err_t net_profiles_forget_all(){const auto snapshot = config_get(); DeviceConfig config = *snapshot;config.wifi.profile_count=0;for(auto& p:config.wifi.profiles)p=WifiProfile{};return config_set(config, snapshot);}
}  // namespace ledcast
