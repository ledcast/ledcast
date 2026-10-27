#include <cstdlib>
#include <cstring>

#include <cJSON.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "deviceconfig.h"
#include "netconfig.h"
#include "webui_internal.h"

namespace ledcast::webui {
namespace {
constexpr size_t kMaxScanResults = 24; constexpr int64_t kScanMinIntervalUs = 3000000;
ScanResult s_results[kMaxScanResults]; int s_found = 0; int64_t s_updated_us = 0; int64_t s_started_us = 0; bool s_scanning = false; bool s_failed = false; SemaphoreHandle_t s_lock = nullptr;
SemaphoreHandle_t scan_lock() { if (!s_lock) s_lock = xSemaphoreCreateMutex(); return s_lock; }
bool take() { auto lock = scan_lock(); return lock && xSemaphoreTake(lock, pdMS_TO_TICKS(200)) == pdTRUE; } void give() { if (s_lock) xSemaphoreGive(s_lock); }
esp_err_t persist(esp_err_t err) { return err == ESP_OK ? config_flush() : err; }
void scan_task(void*) { ScanResult buffer[kMaxScanResults]{}; const int found = net_scan(buffer, kMaxScanResults); if (take()) { s_failed = found < 0; if (found >= 0) { memcpy(s_results, buffer, found * sizeof(ScanResult)); s_found = found; s_updated_us = esp_timer_get_time(); } s_scanning = false; give(); } else s_scanning = false; vTaskDelete(nullptr); }
void kick_scan() { if (!take()) return; const int64_t now = esp_timer_get_time(); const bool ready = !s_scanning && (!s_started_us || now - s_started_us >= kScanMinIntervalUs); if (ready) { s_scanning = true; s_failed = false; s_started_us = now; } give(); if (ready && xTaskCreatePinnedToCore(scan_task, "webui_scan", 4096, nullptr, 3, nullptr, 0) != pdPASS) { if (take()) { s_scanning = false; s_failed = true; give(); } } }
bool scan_poll(httpd_req_t* req) {
    char query[32]{}, value[8]{};
    return httpd_req_get_url_query_str(req,query,sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query,"poll",value,sizeof(value)) == ESP_OK && strcmp(value,"1") == 0;
}
bool profile_tail(httpd_req_t* req, const char* suffix, char out[33]) {
    constexpr const char* prefix = "/api/v1/wifi/profiles/"; const size_t prefix_len = strlen(prefix); if (strncmp(req->uri, prefix, prefix_len) != 0) return false; const char* begin = req->uri + prefix_len; const char* end = suffix ? strstr(begin, suffix) : begin + strcspn(begin, "?#"); if (!end || end == begin || static_cast<size_t>(end - begin) > 32) return false;
    size_t written = 0; for (const char* p = begin; p < end;) { char c = *p++; if (c == '%') { if (p + 1 >= end) return false; auto hex=[](char x)->int { if(x>='0'&&x<='9')return x-'0'; if(x>='a'&&x<='f')return x-'a'+10; if(x>='A'&&x<='F')return x-'A'+10; return -1; }; int hi=hex(*p++),lo=hex(*p++); if(hi<0||lo<0)return false; c=(hi<<4)|lo; } if (!c || static_cast<unsigned char>(c) < 0x20) return false; out[written++]=c; } out[written]=0; return true;
}
esp_err_t profiles_response(httpd_req_t* req) { const auto snapshot = config_get(); const auto& cfg = *snapshot; const auto net = net_get_status(); char buffer[kJsonSinkBuffer]; JsonSink json = make_http_sink(req, buffer, sizeof(buffer)); json.object_open(); json.array_open("profiles"); for (uint8_t i=0;i<cfg.wifi.profile_count;++i) { json.object_open(); json.string("id", cfg.wifi.profiles[i].ssid); json.string("ssid", cfg.wifi.profiles[i].ssid); json.boolean("active", strcmp(cfg.wifi.profiles[i].ssid, net.ssid)==0); json.object_close(); } json.array_close(); json.object_close(); return finish_http_sink(req,json); }
}  // namespace

esp_err_t handle_wifi_scan(httpd_req_t* req) { if (!scan_poll(req)) kick_scan(); ScanResult snapshot[kMaxScanResults]{}; int found=0; bool scanning=true,failed=false; int32_t age=-1; if(take()){ found=s_found; scanning=s_scanning; failed=s_failed; memcpy(snapshot,s_results,found*sizeof(ScanResult)); if(s_updated_us)age=(esp_timer_get_time()-s_updated_us)/1000; give(); } char buffer[kJsonSinkBuffer]; JsonSink json=make_http_sink(req,buffer,sizeof(buffer)); json.object_open(); json.array_open("networks"); for(int i=0;i<found;++i){ json.object_open(); json.string("ssid",snapshot[i].ssid); json.integer("rssi",snapshot[i].rssi); json.boolean("secure",snapshot[i].secured); json.boolean("saved",snapshot[i].known); json.object_close(); } json.array_close(); json.boolean("scanning",scanning); json.boolean("failed",failed); json.integer("age_ms",age); json.object_close(); return finish_http_sink(req,json); }
esp_err_t handle_wifi_profiles_get(httpd_req_t* req) { return profiles_response(req); }
esp_err_t handle_wifi_profiles_post(httpd_req_t* req) { size_t len=0; char* body=read_json_body(req,&len); if(!body)return ESP_OK; cJSON* root=cJSON_ParseWithLength(body,len); free(body); const cJSON* ssid=root?cJSON_GetObjectItemCaseSensitive(root,"ssid"):nullptr; const cJSON* password=root?cJSON_GetObjectItemCaseSensitive(root,"password"):nullptr; const cJSON* connect=root?cJSON_GetObjectItemCaseSensitive(root,"connect"):nullptr; bool keys=true; if(root){const cJSON* i=nullptr;cJSON_ArrayForEach(i,root)keys &= strcmp(i->string,"ssid")==0||strcmp(i->string,"password")==0||strcmp(i->string,"connect")==0;} if(!root||!keys||!cJSON_IsString(ssid)||!ssid->valuestring||strlen(ssid->valuestring)<1||strlen(ssid->valuestring)>32||!cJSON_IsString(password)||!password->valuestring||strlen(password->valuestring)>64||(connect&&!cJSON_IsBool(connect))){cJSON_Delete(root);return send_json_error(req,"400 Bad Request","Недопустимый профиль Wi-Fi");} const esp_err_t err=persist(net_profile_upsert(ssid->valuestring,password->valuestring)); cJSON_Delete(root); if(err!=ESP_OK)return send_json_result(req,err,"Не сохранил профиль"); return profiles_response(req); }
esp_err_t handle_wifi_profile_delete(httpd_req_t* req) { char ssid[33]; if(!profile_tail(req,nullptr,ssid))return send_json_error(req,"400 Bad Request","Неизвестный профиль"); return send_json_result(req,persist(net_profile_forget(ssid)),"Не удалил профиль"); }
esp_err_t handle_wifi_profiles_delete(httpd_req_t* req) { return send_json_result(req,persist(net_profiles_forget_all()),"Не удалил профили"); }
esp_err_t handle_wifi_profile_connect(httpd_req_t* req) { char ssid[33]; if(!profile_tail(req,"/connect",ssid))return send_json_error(req,"400 Bad Request","Неизвестный профиль"); const esp_err_t err=persist(net_profile_connect(ssid)); if(err==ESP_ERR_NOT_FOUND)return send_json_error(req,"404 Not Found","Профиль не найден"); return send_json_result(req,err,"Не подключил профиль"); }
}  // namespace ledcast::webui
