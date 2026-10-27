#include "webui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cJSON.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/md.h>

#include "deviceconfig.h"
#include "stream_node.h"
#include "netconfig.h"
#include "console_auth.h"
#include "socket_address.h"
#include "webui_internal.h"

namespace ledcast {
namespace {
using namespace ledcast::webui;
constexpr char kTag[] = "webui"; constexpr int64_t kLoginWindowUs = 60000000;
httpd_handle_t s_server = nullptr;
// Обработчики httpd идут в одной задаче: таблицам блокировка не нужна.
SessionTable s_sessions; NonceRing s_nonces;
uint8_t s_login_failures = 0; int64_t s_login_window = 0;

enum class Access : uint8_t { Public, Mutation, Login }; enum class Body : uint8_t { None, Json, Raw };
struct Route { const char* uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t*); Access access; Body body; };
void security_headers(httpd_req_t* req) { httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff"); httpd_resp_set_hdr(req,"X-Frame-Options","DENY"); httpd_resp_set_hdr(req,"Referrer-Policy","no-referrer"); httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; object-src 'none'; frame-ancestors 'none'; base-uri 'none'"); }
bool same_origin(httpd_req_t* req) { char origin[96]; const auto err=httpd_req_get_hdr_value_str(req,"Origin",origin,sizeof(origin)); if(err==ESP_ERR_NOT_FOUND)return true; if(err!=ESP_OK)return false; const char* hostpart=strstr(origin,"://"); hostpart=hostpart?hostpart+3:origin; char host[64]; return httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))==ESP_OK && strcmp(hostpart,host)==0; }
bool is_ap_request(httpd_req_t* req) {
    sockaddr_storage local{}; socklen_t len=sizeof(local);
    if(getsockname(httpd_req_to_sockfd(req),reinterpret_cast<sockaddr*>(&local),&len)!=0)return false;
    return socket_has_ipv4(local,net_get_status().ap_ip);
}
// Действующий пароль строки: заданный для веб-интерфейса, иначе пароль точки доступа.
const char* effective_password(const DeviceConfig& cfg) { return cfg.security.web_password[0]?cfg.security.web_password:cfg.wifi.ap_password; }
bool admin_password_matches(const char* password) { const auto snapshot=config_get(); const auto& cfg=*snapshot; return secure_equal(password,effective_password(cfg)); }
bool hmac_sha256(const uint8_t* key,size_t key_len,const uint8_t* message,size_t message_len,uint8_t out[kAuthKeyBytes]) {
    const mbedtls_md_info_t* info=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info&&mbedtls_md_hmac(info,key,key_len,message,message_len,out)==0;
}
bool bearer_ok(httpd_req_t* req) {
    char header[160];
    const int64_t now=esp_timer_get_time();
    if(httpd_req_get_hdr_value_str(req,"Authorization",header,sizeof(header))==ESP_OK && strncmp(header,"Bearer ",7)==0) {
        if(s_sessions.check(header+7,now))return true;
    }
    if(httpd_req_get_hdr_value_str(req,"Cookie",header,sizeof(header))!=ESP_OK)return false;
    const char* cookie=strstr(header,"ledcast_session=");
    if(!cookie)return false;
    cookie+=strlen("ledcast_session=");
    char value[65]{}; size_t n=strcspn(cookie,"; "); if(n!=64)return false;
    memcpy(value,cookie,n); return s_sessions.check(value,now);
}
// Сессия входа; с адреса вошедшего клиента строка принимает поток, пока сессия жива.
const char* open_session(httpd_req_t* req) {
    uint8_t bytes[kAuthHexChars/2]; esp_fill_random(bytes,sizeof(bytes));
    const int64_t now=esp_timer_get_time();
    sockaddr_storage peer{}; socklen_t len=sizeof(peer); uint32_t ipv4=0;
    if(getpeername(httpd_req_to_sockfd(req),reinterpret_cast<sockaddr*>(&peer),&len)==0&&socket_ipv4(peer,ipv4))
        stream_allow_sender(ipv4,now+SessionTable::kLifetimeUs);
    return s_sessions.open(bytes,now);
}
esp_err_t auth_error(httpd_req_t* req) { httpd_resp_set_status(req,"401 Unauthorized"); return send_json_error(req,"401 Unauthorized","Требуется пароль устройства"); }
// Пароль и вход Console делят один счётчик неудач: перебор не обходит лимит сменой входа.
bool login_limited(httpd_req_t* req) { const int64_t now=esp_timer_get_time(); if(now-s_login_window>kLoginWindowUs){s_login_window=now;s_login_failures=0;} if(s_login_failures<5)return false; httpd_resp_set_hdr(req,"Retry-After","60"); send_json_error(req,"429 Too Many Requests","Слишком много попыток входа"); return true; }
esp_err_t send_session(httpd_req_t* req,const char* token) { char reply[112]; snprintf(reply,sizeof(reply),"{\"token\":\"%s\",\"expires_in\":%d}",token,SessionTable::kLifetimeSec); httpd_resp_set_type(req, "application/json; charset=utf-8"); return httpd_resp_send(req,reply,HTTPD_RESP_USE_STRLEN); }
esp_err_t handle_login(httpd_req_t* req) { if(login_limited(req))return ESP_OK; size_t len=0;char* body=read_json_body(req,&len);if(!body)return ESP_OK;cJSON* root=cJSON_ParseWithLength(body,len);free(body);const cJSON* password=root?cJSON_GetObjectItemCaseSensitive(root,"password"):nullptr; bool only=true;if(root){const cJSON*i=nullptr;cJSON_ArrayForEach(i,root)only&=strcmp(i->string,"password")==0;} if(!root||!only||!cJSON_IsString(password)||!password->valuestring){cJSON_Delete(root);return send_json_error(req,"400 Bad Request","Ожидается пароль");} const bool ok=admin_password_matches(password->valuestring);cJSON_Delete(root);if(!ok){++s_login_failures;return auth_error(req);} s_login_failures=0;const char* token=open_session(req);char cookie[144];snprintf(cookie,sizeof(cookie),"ledcast_session=%s; Path=/; Max-Age=43200; HttpOnly; SameSite=Strict",token);httpd_resp_set_hdr(req,"Set-Cookie",cookie);return send_session(req,token); }
esp_err_t handle_challenge(httpd_req_t* req) {
    uint8_t bytes[kAuthHexChars/2]; esp_fill_random(bytes,sizeof(bytes));
    char reply[112]; snprintf(reply,sizeof(reply),"{\"nonce\":\"%s\",\"expires_in\":%d}",s_nonces.issue(bytes,esp_timer_get_time()),NonceRing::kLifetimeSec);
    httpd_resp_set_type(req, "application/json; charset=utf-8"); httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_send(req,reply,HTTPD_RESP_USE_STRLEN);
}
// Вход Console по ключу, выведенному из пароля: {nonce, proof, console}. Nonce гасится при
// любом исходе, неудача считается в общий лимит входа.
esp_err_t handle_console_login(httpd_req_t* req) {
    if(login_limited(req))return ESP_OK;
    size_t len=0; char* body=read_json_body(req,&len); if(!body)return ESP_OK;
    cJSON* root=cJSON_ParseWithLength(body,len); free(body);
    const cJSON* nonce=root?cJSON_GetObjectItemCaseSensitive(root,"nonce"):nullptr;
    const cJSON* proof=root?cJSON_GetObjectItemCaseSensitive(root,"proof"):nullptr;
    const cJSON* console=root?cJSON_GetObjectItemCaseSensitive(root,"console"):nullptr;
    bool known=true;
    if(root){const cJSON* i=nullptr; cJSON_ArrayForEach(i,root)known&=strcmp(i->string,"nonce")==0||strcmp(i->string,"proof")==0||strcmp(i->string,"console")==0;}
    if(!root||!known||!cJSON_IsString(nonce)||!cJSON_IsString(proof)||(console&&!cJSON_IsString(console))){cJSON_Delete(root);return send_json_error(req,"400 Bad Request","Ожидаются nonce и proof");}
    bool ok=s_nonces.consume(nonce->valuestring,esp_timer_get_time());
    if(ok){
        uint8_t key[kAuthKeyBytes]; char id[18]; device_id(id);
        const auto snapshot=config_get();
        ok=console_key(hmac_sha256,effective_password(*snapshot),id,key)&&console_proof_ok(hmac_sha256,key,nonce->valuestring,proof->valuestring);
        memset(key,0,sizeof(key));
    }
    if(ok)ESP_LOGI(kTag,"Console вошла: %.32s",console?console->valuestring:"");
    cJSON_Delete(root);
    if(!ok){++s_login_failures;return auth_error(req);}
    s_login_failures=0; return send_session(req,open_session(req));
}
esp_err_t handle_session(httpd_req_t* req) { httpd_resp_set_type(req, "application/json; charset=utf-8"); return httpd_resp_send(req,(is_ap_request(req)||bearer_ok(req))?"{\"authenticated\":true}":"{\"authenticated\":false}",HTTPD_RESP_USE_STRLEN); }
bool body_type_ok(httpd_req_t* req,Body body){
    if(body==Body::None)return req->content_len==0;
    char type[64];
    if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK)return false;
    return body==Body::Raw ? strncasecmp(type,"application/octet-stream",24)==0
                           : strncasecmp(type,"application/json",16)==0;
}
bool memory_tight(){return http_admission(esp_get_free_heap_size())==HttpAdmission::Reject;}
esp_err_t guarded(httpd_req_t* req){const auto* route=static_cast<const Route*>(req->user_ctx);security_headers(req);if(memory_tight())return send_json_error(req,"503 Service Unavailable","Недостаточно свободной памяти");if(!same_origin(req))return send_json_error(req,"403 Forbidden","Запрос с другого источника запрещён");if(!body_type_ok(req,route->body))return send_json_error(req,"415 Unsupported Media Type","Недопустимый тип тела запроса");if(route->access==Access::Mutation&&!is_ap_request(req)&&!bearer_ok(req))return auth_error(req);return route->handler(req);}
extern const uint8_t kEmbeddedUiStart[] asm("_binary_index_html_gz_start"); extern const uint8_t kEmbeddedUiEnd[] asm("_binary_index_html_gz_end");
esp_err_t handle_ui(httpd_req_t* req){
    security_headers(req);
    // OS captive checks use arbitrary HTTP hosts/paths. Canonicalize only AP
    // traffic so relative API requests and session cookies share one origin.
    if (is_ap_request(req)) {
        char host[64]{};
        const bool canonical = httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host)) == ESP_OK &&
            (strcmp(host,"192.168.4.1") == 0 || strcmp(host,"192.168.4.1:80") == 0);
        if (!canonical || (strcmp(req->uri,"/") != 0 && req->uri[1] != '?')) {
            httpd_resp_set_status(req,"302 Found");
            httpd_resp_set_hdr(req,"Location","http://192.168.4.1/");
            httpd_resp_set_hdr(req,"Cache-Control","no-store");
            return httpd_resp_send(req,"",0);
        }
    }
    const size_t size=kEmbeddedUiEnd-kEmbeddedUiStart;if(!size)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"LEDCAST UI missing");httpd_resp_set_type(req,"text/html; charset=utf-8");httpd_resp_set_hdr(req,"Content-Encoding","gzip");httpd_resp_set_hdr(req,"Cache-Control","no-store");return httpd_resp_send(req,reinterpret_cast<const char*>(kEmbeddedUiStart),size);
}
constexpr Route kRoutes[]={
 {"/api/v1/status",HTTP_GET,handle_status,Access::Public,Body::None},{"/api/v1/hardware",HTTP_GET,handle_hardware_get,Access::Public,Body::None},{"/api/v1/hardware",HTTP_PUT,handle_hardware_put,Access::Mutation,Body::Json},
 {"/api/v1/brightness",HTTP_PUT,handle_brightness_put,Access::Mutation,Body::Json},
 {"/api/v1/show/enabled",HTTP_PUT,handle_show_enabled_put,Access::Mutation,Body::Json},
 {"/api/v1/show",HTTP_GET,handle_show_get,Access::Public,Body::None},{"/api/v1/show",HTTP_PUT,handle_show_put,Access::Mutation,Body::Json},
 {"/api/v1/system",HTTP_GET,handle_system_get,Access::Public,Body::None},{"/api/v1/system",HTTP_PUT,handle_system_put,Access::Mutation,Body::Json},{"/api/v1/wifi/scan",HTTP_GET,handle_wifi_scan,Access::Public,Body::None},
 {"/api/v1/wifi/profiles",HTTP_GET,handle_wifi_profiles_get,Access::Public,Body::None},{"/api/v1/wifi/profiles",HTTP_POST,handle_wifi_profiles_post,Access::Mutation,Body::Json},{"/api/v1/wifi/profiles",HTTP_DELETE,handle_wifi_profiles_delete,Access::Mutation,Body::None},
 {"/api/v1/wifi/profiles/*",HTTP_POST,handle_wifi_profile_connect,Access::Mutation,Body::Json},{"/api/v1/wifi/profiles/*",HTTP_DELETE,handle_wifi_profile_delete,Access::Mutation,Body::None},
 {"/api/v1/auth/login",HTTP_POST,handle_login,Access::Login,Body::Json},{"/api/v1/auth/session",HTTP_GET,handle_session,Access::Public,Body::None},
 {"/api/v1/auth/challenge",HTTP_GET,handle_challenge,Access::Public,Body::None},{"/api/v1/auth/console",HTTP_POST,handle_console_login,Access::Login,Body::Json},
 {"/api/v1/test",HTTP_POST,handle_test,Access::Mutation,Body::Json},{"/api/v1/ota",HTTP_POST,handle_ota,Access::Mutation,Body::Raw},{"/api/v1/reboot",HTTP_POST,handle_reboot,Access::Mutation,Body::Json},{"/api/v1/factory-reset",HTTP_POST,handle_factory_reset,Access::Mutation,Body::Json},};
esp_err_t register_routes(){for(const auto& entry:kRoutes){httpd_uri_t route{};route.uri=entry.uri;route.method=entry.method;route.handler=guarded;route.user_ctx=const_cast<Route*>(&entry);ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server,&route),kTag,"route %s",entry.uri);}httpd_uri_t ui{};ui.uri="/*";ui.method=HTTP_GET;ui.handler=handle_ui;return httpd_register_uri_handler(s_server,&ui);}
void close_socket(httpd_handle_t,int fd){close(fd);}
}  // namespace

esp_err_t webui_start(){if(s_server)return ESP_OK;httpd_config_t config=HTTPD_DEFAULT_CONFIG();config.core_id=0;config.task_priority=4;config.stack_size=8192;config.max_uri_handlers=26;config.max_open_sockets=7;config.lru_purge_enable=true;config.backlog_conn=5;config.max_req_hdr_len=kMaxRequestHeaderBytes;config.uri_match_fn=httpd_uri_match_wildcard;config.close_fn=close_socket;config.recv_wait_timeout=kRecvWaitTimeoutSec;config.send_wait_timeout=kSendWaitTimeoutSec;ESP_RETURN_ON_ERROR(httpd_start(&s_server,&config),kTag,"httpd_start");const auto err=register_routes();if(err!=ESP_OK){httpd_stop(s_server);s_server=nullptr;return err;}ESP_LOGI(kTag,"LEDCAST API v1 started");return ESP_OK;}
esp_err_t webui_rebind(){if(s_server){const auto err=httpd_stop(s_server);s_server=nullptr;if(err!=ESP_OK)return err;}return webui_start();}
}  // namespace ledcast
