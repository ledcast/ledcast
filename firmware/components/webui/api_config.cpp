#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <cJSON.h>
#include <esp_mac.h>

#include "deviceconfig.h"
#include "netconfig.h"
#include "webui_internal.h"

namespace ledcast::webui {
namespace {

const char* color_name(ColorOrder value) {
    static constexpr const char* names[] = {"RGB", "RBG", "GRB", "GBR", "BRG", "BGR"};
    const auto i = static_cast<unsigned>(value);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "RGB";
}
bool color_value(const char* name, ColorOrder& out) {
    static constexpr const char* names[] = {"RGB", "RBG", "GRB", "GBR", "BRG", "BGR"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (strcmp(name, names[i]) == 0) { out = static_cast<ColorOrder>(i); return true; }
    return false;
}
const char* layout_name(ScanLayout value) {
    static constexpr const char* names[] = {
        "auto", "two-scan", "four-scan-16", "four-scan-32", "four-scan-40",
        "four-scan-64", "p10-base8", "p10-base8-plain", "p10-base16",
        "p10-quarter", "kaler-2scan", "stripe", "flipped-stripe", "checkered",
        "spiral", "zstripe", "zstripe-uneven", "coreman"
    };
    const auto i = static_cast<unsigned>(value);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "auto";
}
bool layout_value(const char* name, ScanLayout& out) {
    static constexpr const char* names[] = {
        "auto", "two-scan", "four-scan-16", "four-scan-32", "four-scan-40",
        "four-scan-64", "p10-base8", "p10-base8-plain", "p10-base16",
        "p10-quarter", "kaler-2scan", "stripe", "flipped-stripe", "checkered",
        "spiral", "zstripe", "zstripe-uneven", "coreman"
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0) { out = static_cast<ScanLayout>(i); return true; }
    }
    return false;
}
const char* driver_name(ShiftDriver value) {
    static constexpr const char* names[] = {"generic", "icn2038s", "fm6124", "fm6126a", "mbi5124", "dp3246"};
    const auto i = static_cast<unsigned>(value);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "generic";
}
bool driver_value(const char* name, ShiftDriver& out) {
    static constexpr const char* names[] = {"generic", "icn2038s", "fm6124", "fm6126a", "mbi5124", "dp3246"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0) { out = static_cast<ShiftDriver>(i); return true; }
    }
    return false;
}
const char* decoder_name(LineDecoder value) {
    static constexpr const char* names[] = {"138", "direct", "sm5266p", "sm5368"};
    const auto i = static_cast<unsigned>(value);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "138";
}
bool decoder_value(const char* name, LineDecoder& out) {
    static constexpr const char* names[] = {"138", "direct", "sm5266p", "sm5368"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0) { out = static_cast<LineDecoder>(i); return true; }
    }
    return false;
}
bool exact_keys(const cJSON* object, const char* const* allowed, size_t count) {
    if (!cJSON_IsObject(object)) return false;
    const cJSON* item = nullptr; cJSON_ArrayForEach(item, object) { bool found = false; for (size_t i = 0; i < count; ++i) found |= strcmp(item->string, allowed[i]) == 0; if (!found) return false; }
    return true;
}
bool integer(const cJSON* object, const char* key, int min, int max, int& out) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble != value->valueint || value->valueint < min || value->valueint > max) return false;
    out = value->valueint; return true;
}
esp_err_t send_hardware(httpd_req_t* req) {
    const auto snapshot = config_get(); const auto& cfg = *snapshot; char buffer[kJsonSinkBuffer]; JsonSink json = make_http_sink(req, buffer, sizeof(buffer));
    json.object_open(); json.object_open("module"); json.integer("width", cfg.line.module.width); json.integer("height", cfg.line.module.height);
    json.integer("scan_divisor", cfg.line.module.scan_divisor); json.string("layout", layout_name(cfg.line.module.scan_layout)); json.string("color_order", color_name(cfg.line.module.color_order));
    json.string("shift_driver", driver_name(cfg.line.module.shift_driver)); json.string("line_decoder", decoder_name(cfg.line.module.line_decoder));
    json.boolean("oe_active_low", cfg.line.module.oe_active_low); json.boolean("data_active_high", cfg.line.module.data_active_high); json.object_close();
    json.integer("module_count", cfg.line.module_count); json.array_open("modules");
    for (uint8_t i = 0; i < cfg.line.module_count; ++i) { json.object_open(); json.integer("physical", module_label(cfg.line.modules[i].physical_module)); json.boolean("rotate_180", cfg.line.modules[i].rotate_180); json.boolean("mirror_x", cfg.line.modules[i].mirror_x); json.boolean("mirror_y", cfg.line.modules[i].mirror_y); json.object_close(); }
    json.array_close(); json.object_open("display"); json.integer("brightness", cfg.display.brightness); json.integer("color_depth", cfg.display.bit_depth); json.integer("dma_buffers", cfg.display.buffers); json.integer("bus_hz", cfg.display.bus_hz);
    json.boolean("clk_phase", cfg.display.clk_phase); json.integer("latch_blanking", cfg.display.latch_blanking); json.integer("frame_rate", cfg.display.frame_rate); json.object_close(); json.object_close();
    return finish_http_sink(req, json);
}
bool parse_hardware(const cJSON* root, DeviceConfig& cfg) {
    static constexpr const char* root_keys[] = {"module", "module_count", "modules", "display"}; static constexpr const char* module_keys[] = {"type", "width", "height", "scan_divisor", "layout", "color_order", "shift_driver", "line_decoder", "oe_active_low", "data_active_high"};
    static constexpr const char* item_keys[] = {"physical", "rotate_180", "mirror_x", "mirror_y"}; static constexpr const char* display_keys[] = {"brightness", "color_depth", "dma_buffers", "bus_hz", "clk_phase", "latch_blanking", "frame_rate"};
    if (!exact_keys(root, root_keys, 4)) return false;
    const cJSON* module = cJSON_GetObjectItemCaseSensitive(root, "module");
    const cJSON* modules = cJSON_GetObjectItemCaseSensitive(root, "modules");
    const cJSON* display = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (!exact_keys(module, module_keys, 10) || !cJSON_IsArray(modules) || !exact_keys(display, display_keys, 7)) return false;
    int value = 0;
    if (!integer(root, "module_count", 1, kMaxLineModules, value)) return false;
    cfg.line.module_count = value;
    if (cJSON_GetArraySize(modules) != value) return false;
    if (!integer(module, "width", 8, 256, value)) return false;
    cfg.line.module.width = value;
    if (!integer(module, "height", 4, kMaxModuleHeight, value)) return false;
    cfg.line.module.height = value;
    if (!integer(module, "scan_divisor", 1, cfg.line.module.height, value) || cfg.line.module.height % value != 0) return false;
    cfg.line.module.scan_divisor = value;
    const cJSON* layout = cJSON_GetObjectItemCaseSensitive(module, "layout"); const cJSON* color = cJSON_GetObjectItemCaseSensitive(module, "color_order");
    const cJSON* driver = cJSON_GetObjectItemCaseSensitive(module, "shift_driver"); const cJSON* decoder = cJSON_GetObjectItemCaseSensitive(module, "line_decoder");
    const cJSON* oe = cJSON_GetObjectItemCaseSensitive(module, "oe_active_low"); const cJSON* data = cJSON_GetObjectItemCaseSensitive(module, "data_active_high");
    if (!cJSON_IsString(layout) || !layout_value(layout->valuestring, cfg.line.module.scan_layout) || !cJSON_IsString(color) || !color_value(color->valuestring, cfg.line.module.color_order) ||
        !cJSON_IsString(driver) || !driver_value(driver->valuestring, cfg.line.module.shift_driver) || !cJSON_IsString(decoder) || !decoder_value(decoder->valuestring, cfg.line.module.line_decoder) ||
        !cJSON_IsBool(oe) || !cJSON_IsBool(data)) return false;
    cfg.line.module.oe_active_low = cJSON_IsTrue(oe); cfg.line.module.data_active_high = cJSON_IsTrue(data); bool seen[kMaxLineModules]{};
    for (int i = 0; i < cfg.line.module_count; ++i) { const cJSON* item = cJSON_GetArrayItem(modules, i); if (!exact_keys(item, item_keys, 4) || !integer(item, "physical", 1, cfg.line.module_count, value)) return false; const uint8_t physical = value - 1; if (seen[physical]) return false; seen[physical] = true; auto& target = cfg.line.modules[i]; target.physical_module = physical;
        const cJSON* rotate = cJSON_GetObjectItemCaseSensitive(item, "rotate_180"); const cJSON* mx = cJSON_GetObjectItemCaseSensitive(item, "mirror_x"); const cJSON* my = cJSON_GetObjectItemCaseSensitive(item, "mirror_y"); if (!cJSON_IsBool(rotate) || !cJSON_IsBool(mx) || !cJSON_IsBool(my)) return false; target.rotate_180 = cJSON_IsTrue(rotate); target.mirror_x = cJSON_IsTrue(mx); target.mirror_y = cJSON_IsTrue(my); }
    if (!integer(display, "brightness", 0, 255, value)) return false;
    cfg.display.brightness = value;
    if (!integer(display, "color_depth", 2, 8, value)) return false;
    cfg.display.bit_depth = value;
    if (!integer(display, "dma_buffers", 1, 2, value)) return false;
    cfg.display.buffers = value;
    if (!integer(display, "bus_hz", 0, 20000000, value)) return false;
    bool known_bus = false;
    for (const uint32_t bus : kBusFrequencies) known_bus |= bus == static_cast<uint32_t>(value);
    if (!known_bus) return false;
    cfg.display.bus_hz = value;
    const cJSON* phase = cJSON_GetObjectItemCaseSensitive(display, "clk_phase");
    if (!cJSON_IsBool(phase)) return false;
    cfg.display.clk_phase = cJSON_IsTrue(phase);
    if (!integer(display, "latch_blanking", 1, 4, value)) return false;
    cfg.display.latch_blanking = value;
    if (!integer(display, "frame_rate", 0, 255, value) || !config_frame_rate_valid(static_cast<uint8_t>(value))) return false;
    cfg.display.frame_rate = value;
    return true;
}
}  // namespace

esp_err_t handle_hardware_get(httpd_req_t* req) { return send_hardware(req); }
esp_err_t handle_hardware_put(httpd_req_t* req) {
    size_t len = 0; char* body = read_json_body(req, &len); if (!body) return ESP_OK; cJSON* root = cJSON_ParseWithLength(body, len); free(body); const auto snapshot = config_get(); auto* cfg = new (std::nothrow) DeviceConfig(*snapshot);
    if (!root || !cfg || !parse_hardware(root, *cfg)) { cJSON_Delete(root); delete cfg; return send_json_error(req, "400 Bad Request", "Недопустимая конфигурация оборудования"); } cJSON_Delete(root);
    char reason[192]{}; if (config_validate(*cfg, reason, sizeof(reason)) != ESP_OK) { delete cfg; return send_json_error(req, "400 Bad Request", reason[0] ? reason : "Конфигурация не поддерживается"); }
    esp_err_t err = config_set(*cfg, snapshot); delete cfg;
    if (err == ESP_OK) err = config_flush();
    if (err != ESP_OK) return send_json_result(req, err, "Не сохранил конфигурацию");
    return send_hardware(req);
}
esp_err_t handle_brightness_put(httpd_req_t* req) {
    size_t len = 0;
    char* body = read_json_body(req, &len);
    if (!body) return ESP_OK;
    cJSON* root = cJSON_ParseWithLength(body, len);
    free(body);
    static constexpr const char* keys[] = {"value"};
    int value = 0;
    if (!root || !exact_keys(root, keys, 1) || !integer(root, "value", 0, 255, value)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "Яркость должна быть в диапазоне 0..255");
    }
    cJSON_Delete(root);
    const auto snapshot = config_get(); DeviceConfig cfg = *snapshot;
    cfg.display.brightness = static_cast<uint8_t>(value);
    const esp_err_t err = config_set(cfg, snapshot);
    if (err != ESP_OK) return send_json_result(req, err, "Не изменил яркость");
    // config_set applies immediately and coalesces flash writes itself. Do not
    // force a synchronous flash erase for every slider movement.
    char reply[32];
    snprintf(reply, sizeof(reply), "{\"brightness\":%d}", value);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, reply, HTTPD_RESP_USE_STRLEN);
}
esp_err_t handle_system_get(httpd_req_t* req) {
    char id[18]; device_id(id); const auto snapshot = config_get(); const auto& cfg = *snapshot; char buffer[256];
    JsonSink json = make_http_sink(req, buffer, sizeof(buffer)); json.object_open();
    json.string("name", cfg.wifi.hostname[0] ? cfg.wifi.hostname : net_hostname()); json.string("id", id); json.object_close();
    return finish_http_sink(req, json);
}
esp_err_t handle_system_put(httpd_req_t* req) {
    size_t len = 0;
    char* body = read_json_body(req, &len);
    if (!body) return ESP_OK;
    cJSON* root = cJSON_ParseWithLength(body, len);
    free(body);
    static constexpr const char* keys[] = {"name", "password"};
    const cJSON* name = root ? cJSON_GetObjectItemCaseSensitive(root, "name") : nullptr;
    const cJSON* password = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : nullptr;
    const size_t password_len = cJSON_IsString(password) && password->valuestring
                                    ? strlen(password->valuestring)
                                    : 0;
    const bool password_invalid = password &&
        (!cJSON_IsString(password) || !password->valuestring ||
         (password_len != 0 && (password_len < 8 || password_len >= sizeof(DeviceConfig{}.wifi.ap_password))));
    if (!root || !exact_keys(root, keys, 2) || !cJSON_IsString(name) ||
        !name->valuestring ||
        strlen(name->valuestring) >= sizeof(DeviceConfig{}.wifi.hostname) || password_invalid) {
        cJSON_Delete(root);
        // Пустое имя — строка называется своим сетевым именем.
        return send_json_error(req, "400 Bad Request",
                               "Имя: до 31 символа; новый пароль: 8–63 символа");
    }
    const auto snapshot = config_get(); DeviceConfig cfg = *snapshot;
    snprintf(cfg.wifi.hostname, sizeof(cfg.wifi.hostname), "%s", name->valuestring);
    if (password_len != 0) {
        snprintf(cfg.wifi.ap_password, sizeof(cfg.wifi.ap_password), "%s", password->valuestring);
        snprintf(cfg.security.web_password, sizeof(cfg.security.web_password), "%s",
                 password->valuestring);
    }
    cJSON_Delete(root);
    esp_err_t err = config_set(cfg, snapshot);
    if (err == ESP_OK) err = config_flush();
    if (err != ESP_OK) return send_json_result(req, err, "Не сохранил настройки");
    return handle_system_get(req);
}
esp_err_t handle_factory_reset(httpd_req_t* req) { esp_err_t err = config_reset_factory(); if (err == ESP_OK) err = config_flush(); if (err != ESP_OK) return send_json_result(req, err, "Не сбросил настройки"); const esp_err_t restart = schedule_restart(700); if (restart != ESP_OK) return send_json_result(req, restart, "Не назначил перезагрузку"); httpd_resp_set_type(req, "application/json; charset=utf-8"); return httpd_resp_send(req, "{\"ok\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN); }
esp_err_t handle_reboot(httpd_req_t* req) { const esp_err_t flush = config_flush(); if (flush != ESP_OK) return send_json_result(req, flush, "Не сохранил настройки"); const esp_err_t err = schedule_restart(700); if (err != ESP_OK) return send_json_result(req, err, "Не назначил перезагрузку"); httpd_resp_set_type(req, "application/json; charset=utf-8"); return httpd_resp_send(req, "{\"ok\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN); }
}  // namespace ledcast::webui
