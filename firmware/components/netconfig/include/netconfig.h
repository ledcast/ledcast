#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include "deviceconfig.h"

namespace ledcast {
enum class NetState : uint8_t { Down, ApOnly, StaConnecting, StaConnected, ApSta };
struct NetStatus { NetState state{}; char ip[16]{}; char ap_ip[16]{}; int8_t rssi{}; uint8_t sta_clients{}; char ssid[33]{}; char ap_ssid[33]{}; uint16_t disconnect_reason{}; int8_t tx_power_quarter_dbm{}; uint8_t channel{}; uint8_t power_save{}; };
struct ScanResult { char ssid[33]{}; int8_t rssi{}; bool secured{}; bool known{}; };

using NetInterfaceChangedHandler = void (*)();

esp_err_t net_start(const WifiConfig& cfg);
// Автономный запуск: до net_start. Строка поднимает свою точку доступа и не
// уходит в сохранённые сети, пока Wi-Fi не поправят из интерфейса.
void net_force_setup_ap();
esp_err_t net_reconfigure(const WifiConfig& cfg);
void net_set_interface_changed_handler(NetInterfaceChangedHandler handler);
NetStatus net_get_status();
// Имя строки в сети: ledcast-xxxx по MAC, как в её id; отвечает на <имя>.local.
const char* net_hostname();
int net_scan(ScanResult* out, size_t max_results);
esp_err_t net_profile_upsert(const char* ssid, const char* password);
esp_err_t net_profile_connect(const char* ssid);
esp_err_t net_profile_forget(const char* ssid);
esp_err_t net_profiles_forget_all();
}  // namespace ledcast
