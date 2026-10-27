#include "config_serialize.h"

#include <cstdio>
#include <cstring>
#include <cJSON.h>

namespace ledcast {
namespace {

void add_module(cJSON* out, const ModuleType& m) {
    cJSON_AddNumberToObject(out, "width", m.width);
    cJSON_AddNumberToObject(out, "height", m.height);
    cJSON_AddNumberToObject(out, "scanDivisor", m.scan_divisor);
    cJSON_AddNumberToObject(out, "colorOrder", static_cast<int>(m.color_order));
    cJSON_AddBoolToObject(out, "oeActiveLow", m.oe_active_low);
    cJSON_AddBoolToObject(out, "dataActiveHigh", m.data_active_high);
    cJSON_AddNumberToObject(out, "scanLayout", static_cast<int>(m.scan_layout));
    cJSON_AddNumberToObject(out, "shiftDriver", static_cast<int>(m.shift_driver));
    cJSON_AddNumberToObject(out, "lineDecoder", static_cast<int>(m.line_decoder));
}

void number(cJSON* obj, const char* key, uint8_t& value) { if (auto* v = cJSON_GetObjectItemCaseSensitive(obj, key); cJSON_IsNumber(v)) value = static_cast<uint8_t>(v->valueint); }
void number(cJSON* obj, const char* key, uint16_t& value) { if (auto* v = cJSON_GetObjectItemCaseSensitive(obj, key); cJSON_IsNumber(v)) value = static_cast<uint16_t>(v->valueint); }
void number(cJSON* obj, const char* key, uint32_t& value) { if (auto* v = cJSON_GetObjectItemCaseSensitive(obj, key); cJSON_IsNumber(v)) value = static_cast<uint32_t>(v->valuedouble); }
void boolean(cJSON* obj, const char* key, bool& value) { if (auto* v = cJSON_GetObjectItemCaseSensitive(obj, key); cJSON_IsBool(v)) value = cJSON_IsTrue(v); }
void string(cJSON* obj, const char* key, char* value, size_t len) {
    auto* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || v->valuestring == nullptr) return;
    snprintf(value, len, "%s", v->valuestring);
}

void read_module(cJSON* in, ModuleType& m) {
    number(in, "width", m.width); number(in, "height", m.height); number(in, "scanDivisor", m.scan_divisor);
    uint8_t value = static_cast<uint8_t>(m.color_order); number(in, "colorOrder", value); m.color_order = static_cast<ColorOrder>(value);
    boolean(in, "oeActiveLow", m.oe_active_low); boolean(in, "dataActiveHigh", m.data_active_high);
    value = static_cast<uint8_t>(m.scan_layout); number(in, "scanLayout", value); m.scan_layout = static_cast<ScanLayout>(value);
    value = static_cast<uint8_t>(m.shift_driver); number(in, "shiftDriver", value); m.shift_driver = static_cast<ShiftDriver>(value);
    value = static_cast<uint8_t>(m.line_decoder); number(in, "lineDecoder", value); m.line_decoder = static_cast<LineDecoder>(value);
}

void add_line(cJSON* out, const LineConfig& line) {
    cJSON_AddNumberToObject(out, "moduleCount", line.module_count);
    cJSON* module = cJSON_AddObjectToObject(out, "module"); add_module(module, line.module);
    cJSON* modules = cJSON_AddArrayToObject(out, "modules");
    for (uint8_t i = 0; i < line.module_count; ++i) {
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "physicalModule", line.modules[i].physical_module);
        cJSON_AddBoolToObject(entry, "rotate180", line.modules[i].rotate_180);
        cJSON_AddBoolToObject(entry, "mirrorX", line.modules[i].mirror_x);
        cJSON_AddBoolToObject(entry, "mirrorY", line.modules[i].mirror_y);
        cJSON_AddItemToArray(modules, entry);
    }
}

void read_line(cJSON* in, LineConfig& line) {
    number(in, "moduleCount", line.module_count);
    if (auto* module = cJSON_GetObjectItemCaseSensitive(in, "module"); cJSON_IsObject(module)) read_module(module, line.module);
    if (auto* modules = cJSON_GetObjectItemCaseSensitive(in, "modules"); cJSON_IsArray(modules)) {
        int i = 0; cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, modules) { if (i >= kMaxLineModules) break; number(entry, "physicalModule", line.modules[i].physical_module); boolean(entry, "rotate180", line.modules[i].rotate_180); boolean(entry, "mirrorX", line.modules[i].mirror_x); boolean(entry, "mirrorY", line.modules[i].mirror_y); ++i; }
    }
}

void add_output(cJSON* out, const DisplayConfig& display) {
    cJSON_AddNumberToObject(out, "bitDepth", display.bit_depth); cJSON_AddNumberToObject(out, "buffers", display.buffers);
    cJSON_AddNumberToObject(out, "busHz", display.bus_hz); cJSON_AddBoolToObject(out, "clkPhase", display.clk_phase);
    cJSON_AddNumberToObject(out, "latchBlanking", display.latch_blanking); cJSON_AddNumberToObject(out, "brightness", display.brightness);
    cJSON_AddNumberToObject(out, "frameRate", display.frame_rate);
}

void read_output(cJSON* in, DisplayConfig& display) {
    number(in, "bitDepth", display.bit_depth); number(in, "buffers", display.buffers); number(in, "busHz", display.bus_hz);
    boolean(in, "clkPhase", display.clk_phase); number(in, "latchBlanking", display.latch_blanking);
    number(in, "brightness", display.brightness); number(in, "frameRate", display.frame_rate);
}

void add_show(cJSON* out, const ShowConfig& show) {
    cJSON_AddBoolToObject(out, "enabled", show.enabled);
    cJSON_AddNumberToObject(out, "mode", static_cast<int>(show.mode));
    cJSON_AddNumberToObject(out, "fontSizePx", show.font_size_px);
    cJSON_AddNumberToObject(out, "speedPxS", show.speed_px_s);
    cJSON_AddNumberToObject(out, "durationS", show.duration_s);
    cJSON_AddNumberToObject(out, "red", show.red);
    cJSON_AddNumberToObject(out, "green", show.green);
    cJSON_AddNumberToObject(out, "blue", show.blue);
    cJSON_AddStringToObject(out, "text", show.text);
}

void read_show(cJSON* in, ShowConfig& show) {
    boolean(in, "enabled", show.enabled);
    uint8_t mode = static_cast<uint8_t>(show.mode); number(in, "mode", mode); show.mode = static_cast<ShowMode>(mode);
    number(in, "fontSizePx", show.font_size_px);
    number(in, "speedPxS", show.speed_px_s); number(in, "durationS", show.duration_s);
    number(in, "red", show.red); number(in, "green", show.green); number(in, "blue", show.blue);
    string(in, "text", show.text, sizeof(show.text));
}

void add_net(cJSON* out, const WifiConfig& wifi) {
    cJSON_AddStringToObject(out, "apPassword", wifi.ap_password);
    cJSON_AddNumberToObject(out, "apChannel", wifi.ap_channel); cJSON_AddStringToObject(out, "hostname", wifi.hostname);
    cJSON* profiles = cJSON_AddArrayToObject(out, "profiles");
    for (uint8_t i = 0; i < wifi.profile_count; ++i) {
        cJSON* entry = cJSON_CreateObject(); cJSON_AddStringToObject(entry, "ssid", wifi.profiles[i].ssid);
        cJSON_AddStringToObject(entry, "password", wifi.profiles[i].password); cJSON_AddItemToArray(profiles, entry);
    }
}

void read_net(cJSON* in, WifiConfig& wifi) {
    string(in, "apPassword", wifi.ap_password, sizeof(wifi.ap_password)); number(in, "apChannel", wifi.ap_channel);
    string(in, "hostname", wifi.hostname, sizeof(wifi.hostname));
    if (auto* profiles = cJSON_GetObjectItemCaseSensitive(in, "profiles"); cJSON_IsArray(profiles)) { wifi.profile_count = 0; cJSON* entry = nullptr; cJSON_ArrayForEach(entry, profiles) { if (wifi.profile_count >= kWifiProfileLimit) break; auto& p = wifi.profiles[wifi.profile_count++]; string(entry, "ssid", p.ssid, sizeof(p.ssid)); string(entry, "password", p.password, sizeof(p.password)); } }
}

void add_security(cJSON* out, const SecurityConfig& security) {
    cJSON_AddBoolToObject(out, "webAuthEnabled", security.web_auth_enabled); cJSON_AddStringToObject(out, "webUser", security.web_user);
    cJSON_AddStringToObject(out, "webPassword", security.web_password);
}

void read_security(cJSON* in, SecurityConfig& security) {
    boolean(in, "webAuthEnabled", security.web_auth_enabled); string(in, "webUser", security.web_user, sizeof(security.web_user));
    string(in, "webPassword", security.web_password, sizeof(security.web_password));
}

}  // namespace

int config_domain_to_json(ConfigDomain domain, const DeviceConfig& cfg, char* out, size_t out_len) {
    cJSON* root = cJSON_CreateObject(); if (root == nullptr) return -1;
    switch (domain) {
        case ConfigDomain::Net: add_net(root, cfg.wifi); break;
        case ConfigDomain::Security: add_security(root, cfg.security); break;
        case ConfigDomain::Line: add_line(root, cfg.line); break;
        case ConfigDomain::Output: add_output(root, cfg.display); break;
        case ConfigDomain::Show: add_show(root, cfg.show); break;
        // Таймаут потока фиксирован (2 с) и не хранится.
        case ConfigDomain::Stream:
            cJSON_AddBoolToObject(root, "enabled", cfg.stream.enabled); cJSON_AddNumberToObject(root, "port", cfg.stream.port);
            break;
    }
    const bool ok = cJSON_PrintPreallocated(root, out, static_cast<int>(out_len), false); cJSON_Delete(root);
    return ok ? static_cast<int>(strlen(out)) : -1;
}

esp_err_t config_domain_from_json(ConfigDomain domain, const char* json, size_t len, DeviceConfig& out) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    switch (domain) {
        case ConfigDomain::Net: read_net(root, out.wifi); break;
        case ConfigDomain::Security: read_security(root, out.security); break;
        case ConfigDomain::Line: read_line(root, out.line); break;
        case ConfigDomain::Output: read_output(root, out.display); break;
        case ConfigDomain::Show: read_show(root, out.show); break;
        case ConfigDomain::Stream: boolean(root, "enabled", out.stream.enabled); number(root, "port", out.stream.port); break;
    }
    cJSON_Delete(root); return ESP_OK;
}

esp_err_t config_legacy_from_json(const char* json, size_t len, DeviceConfig& out) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    if (auto* v = cJSON_GetObjectItemCaseSensitive(root, "wifi"); cJSON_IsObject(v)) read_net(v, out.wifi);
    if (auto* v = cJSON_GetObjectItemCaseSensitive(root, "security"); cJSON_IsObject(v)) read_security(v, out.security);
    cJSON_Delete(root); return ESP_OK;
}

}  // namespace ledcast
