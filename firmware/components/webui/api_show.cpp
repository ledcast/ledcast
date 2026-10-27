#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <cJSON.h>

#include "deviceconfig.h"
#include "webui_internal.h"

namespace ledcast::webui {
namespace {

bool exact_keys(const cJSON* object) {
    static constexpr const char* allowed[] = {"enabled", "mode", "font_size_px", "speed_px_s", "duration_s", "color", "text"};
    if (!cJSON_IsObject(object)) return false;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, object) {
        bool found = false;
        for (const char* key : allowed) found |= item->string != nullptr && strcmp(item->string, key) == 0;
        if (!found) return false;
    }
    return true;
}

bool parse_color(const char* value, uint8_t& r, uint8_t& g, uint8_t& b) {
    unsigned red = 0, green = 0, blue = 0;
    if (value == nullptr || strlen(value) != 7 || value[0] != '#' ||
        sscanf(value + 1, "%02x%02x%02x", &red, &green, &blue) != 3) return false;
    r = static_cast<uint8_t>(red); g = static_cast<uint8_t>(green); b = static_cast<uint8_t>(blue);
    return true;
}

int max_font_size_px(const DeviceConfig& config) {
    return std::max(7, std::min(56, static_cast<int>(config.line.module.height)));
}

esp_err_t send_show(httpd_req_t* req) {
    const auto snapshot = config_get();
    const ShowConfig& show = snapshot->show;
    char color[8]; snprintf(color, sizeof(color), "#%02X%02X%02X", show.red, show.green, show.blue);
    char buffer[kJsonSinkBuffer];
    JsonSink json = make_http_sink(req, buffer, sizeof(buffer));
    json.object_open();
    json.boolean("enabled", show.enabled);
    json.string("mode", show.mode == ShowMode::Static ? "static" : "scroll");
    json.integer("font_size_px", std::min<int>(show.font_size_px, max_font_size_px(*snapshot)));
    json.integer("max_font_size_px", max_font_size_px(*snapshot));
    json.integer("speed_px_s", show.speed_px_s);
    json.integer("duration_s", show.duration_s);
    json.string("color", color);
    json.string("text", show.text);
    json.object_close();
    return finish_http_sink(req, json);
}

}  // namespace

esp_err_t handle_show_get(httpd_req_t* req) { return send_show(req); }

// Console playback control, separate from show editing; needs a session like any mutation.
esp_err_t handle_show_enabled_put(httpd_req_t* req) {
    size_t len = 0;
    char* body = read_json_body(req, &len);
    if (!body) return ESP_OK;
    cJSON* root = cJSON_ParseWithLength(body, len);
    free(body);
    const cJSON* value = root ? cJSON_GetObjectItemCaseSensitive(root, "enabled") : nullptr;
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 1 || !cJSON_IsBool(value)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "Ожидается только enabled: true или false");
    }
    const bool enabled = cJSON_IsTrue(value);
    cJSON_Delete(root);
    const auto snapshot = config_get();
    if (snapshot->show.enabled != enabled) {
        auto* next = new (std::nothrow) DeviceConfig(*snapshot);
        if (!next) return send_json_result(req, ESP_ERR_NO_MEM, "Не переключил Show");
        next->show.enabled = enabled;
        const esp_err_t error = config_set(*next, snapshot);
        delete next;
        if (error != ESP_OK) return send_json_result(req, error, "Не переключил Show");
    }
    char buffer[kJsonSinkBuffer];
    JsonSink json = make_http_sink(req, buffer, sizeof(buffer));
    json.object_open();
    json.boolean("enabled", config_get()->show.enabled);
    json.object_close();
    return finish_http_sink(req, json);
}

esp_err_t handle_show_put(httpd_req_t* req) {
    size_t len = 0;
    char* body = read_json_body(req, &len);
    if (body == nullptr) return ESP_OK;
    cJSON* root = cJSON_ParseWithLength(body, len);
    free(body);
    const auto snapshot = config_get(); auto* next = new (std::nothrow) DeviceConfig(*snapshot);
    const cJSON* enabled = root ? cJSON_GetObjectItemCaseSensitive(root, "enabled") : nullptr;
    const cJSON* mode = root ? cJSON_GetObjectItemCaseSensitive(root, "mode") : nullptr;
    const cJSON* font_size = root ? cJSON_GetObjectItemCaseSensitive(root, "font_size_px") : nullptr;
    const cJSON* speed = root ? cJSON_GetObjectItemCaseSensitive(root, "speed_px_s") : nullptr;
    const cJSON* duration = root ? cJSON_GetObjectItemCaseSensitive(root, "duration_s") : nullptr;
    const cJSON* color = root ? cJSON_GetObjectItemCaseSensitive(root, "color") : nullptr;
    const cJSON* text = root ? cJSON_GetObjectItemCaseSensitive(root, "text") : nullptr;
    bool valid = root && next && exact_keys(root) && cJSON_IsBool(enabled) && cJSON_IsString(mode) &&
        cJSON_IsNumber(font_size) && font_size->valuedouble == font_size->valueint &&
        font_size->valueint >= 7 && font_size->valueint <= max_font_size_px(*next) &&
        cJSON_IsNumber(speed) && speed->valuedouble == speed->valueint && speed->valueint >= 1 && speed->valueint <= 240 &&
        cJSON_IsNumber(duration) && duration->valuedouble == duration->valueint && duration->valueint >= 0 && duration->valueint <= 3600 &&
        cJSON_IsString(color) && cJSON_IsString(text) && text->valuestring != nullptr && strlen(text->valuestring) <= kShowTextBytes;
    if (valid) {
        const bool still = strcmp(mode->valuestring, "static") == 0;
        valid = still || strcmp(mode->valuestring, "scroll") == 0;
        valid &= !still || duration->valueint >= 1;
        valid &= parse_color(color->valuestring, next->show.red, next->show.green, next->show.blue);
    }
    if (!valid) {
        cJSON_Delete(root); delete next;
        return send_json_error(req, "400 Bad Request", "Недопустимые настройки автономного показа");
    }
    next->show.enabled = cJSON_IsTrue(enabled);
    next->show.mode = strcmp(mode->valuestring, "static") == 0 ? ShowMode::Static : ShowMode::ScrollRightToLeft;
    next->show.font_size_px = static_cast<uint8_t>(font_size->valueint);
    next->show.speed_px_s = static_cast<uint16_t>(speed->valueint);
    next->show.duration_s = static_cast<uint16_t>(duration->valueint);
    snprintf(next->show.text, sizeof(next->show.text), "%s", text->valuestring);
    cJSON_Delete(root);
    const esp_err_t err = config_set(*next, snapshot);
    delete next;
    if (err != ESP_OK) return send_json_result(req, err, "Не применил автономный показ");
    return send_show(req);
}

}  // namespace ledcast::webui
