#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <cJSON.h>

#include "deviceconfig.h"
#include "display.h"
#include "linear.h"
#include "show.h"
#include "webui.h"
#include "webui_internal.h"

namespace ledcast {
namespace {

enum class TestPattern : uint8_t { None, Identify, Grid, Red, Green, Blue, Gray };
std::atomic<TestPattern> s_pattern{TestPattern::None};

TestPattern pattern_value(const char* name) {
    if (!name) return TestPattern::None;
    if (!strcmp(name, "identify")) return TestPattern::Identify;
    if (!strcmp(name, "grid")) return TestPattern::Grid;
    if (!strcmp(name, "red")) return TestPattern::Red;
    if (!strcmp(name, "green")) return TestPattern::Green;
    if (!strcmp(name, "blue")) return TestPattern::Blue;
    if (!strcmp(name, "gray")) return TestPattern::Gray;
    return TestPattern::None;
}

bool digit_pixel(uint8_t digit, uint8_t x, uint8_t y) {
    static constexpr uint16_t glyphs[10] = {
        0b111101101101111, 0b010110010010111, 0b111001111100111, 0b111001111001111,
        0b101101111001001, 0b111100111001111, 0b111100111101111, 0b111001001001001,
        0b111101111101111, 0b111101111001111,
    };
    return x < 3 && y < 5 && ((glyphs[digit % 10] >> (14 - (y * 3 + x))) & 1u) != 0;
}

bool number_pixel(uint8_t number, uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    const uint8_t digits = number >= 10 ? 2 : 1;
    const uint8_t scale_x = static_cast<uint8_t>((width > 4 ? width - 4 : width) / (digits * 3 + digits - 1));
    const uint8_t scale_y = static_cast<uint8_t>((height > 4 ? height - 4 : height) / 5);
    uint8_t scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale == 0) scale = 1;
    const uint16_t glyph_width = (digits * 3 + digits - 1) * scale;
    const uint16_t glyph_height = 5 * scale;
    const uint16_t left = width > glyph_width ? (width - glyph_width) / 2 : 0;
    const uint16_t top = height > glyph_height ? (height - glyph_height) / 2 : 0;
    if (x < left || y < top || x >= left + glyph_width || y >= top + glyph_height) return false;
    const uint16_t gx = (x - left) / scale;
    const uint8_t slot = static_cast<uint8_t>(gx / 4);
    const uint8_t column = static_cast<uint8_t>(gx % 4);
    if (slot >= digits || column >= 3) return false;
    const uint8_t digit = digits == 2 && slot == 0 ? number / 10 : number % 10;
    return digit_pixel(digit, column, static_cast<uint8_t>((y - top) / scale));
}

esp_err_t paint_pattern(TestPattern pattern) {
    if (!display_test_frame_begin()) return ESP_ERR_TIMEOUT;
    constexpr uint16_t chunk = 64;
    uint8_t rgb[chunk * 3];
    const uint16_t width = linear_width();
    const uint16_t height = linear_height();
    const uint16_t module_width = linear_module_width();
    for (uint16_t y = 0; y < height; ++y) {
        for (uint16_t x = 0; x < width; x += chunk) {
            const uint16_t count = x + chunk <= width ? chunk : width - x;
            for (uint16_t i = 0; i < count; ++i) {
                const uint16_t px = x + i;
                uint8_t r = 0, g = 0, b = 0;
                if (pattern == TestPattern::Red) r = 255;
                else if (pattern == TestPattern::Green) g = 255;
                else if (pattern == TestPattern::Blue) b = 255;
                else if (pattern == TestPattern::Gray) r = g = b = static_cast<uint8_t>((static_cast<uint32_t>(px) * 255) / (width > 1 ? width - 1 : 1));
                else if (pattern == TestPattern::Grid) {
                    const uint16_t local_x = module_width ? px % module_width : px;
                    const bool border = local_x == 0 || local_x + 1 == module_width || y == 0 || y + 1 == height;
                    const bool cell = (local_x % 8u == 0) || (y % 8u == 0);
                    if (border) r = g = b = 255;
                    else if (cell) { g = 72; b = 104; }
                } else if (pattern == TestPattern::Identify) {
                    const uint8_t slot = module_width ? static_cast<uint8_t>(px / module_width) : 0;
                    // The pattern verifies the configured visual order.  The
                    // previous implementation printed the physical chain id,
                    // so dragging cards could never change what was shown and
                    // made a working 3,2,1 controller-right chain look broken.
                    const uint8_t label = static_cast<uint8_t>(slot + 1);
                    const uint16_t local_x = module_width ? px % module_width : px;
                    const bool edge = local_x == 0 || local_x + 1 == module_width || y == 0 || y + 1 == height;
                    const bool glyph = number_pixel(label, local_x, y, module_width, height);
                    static constexpr uint8_t colors[6][3] = {{50, 12, 12}, {10, 42, 18}, {10, 22, 50}, {46, 35, 8}, {8, 38, 40}, {40, 10, 42}};
                    const auto& color = colors[slot % 6];
                    r = glyph ? 255 : edge ? color[0] : static_cast<uint8_t>(color[0] / 5);
                    g = glyph ? 255 : edge ? color[1] : static_cast<uint8_t>(color[1] / 5);
                    b = glyph ? 255 : edge ? color[2] : static_cast<uint8_t>(color[2] / 5);
                }
                rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
            }
            linear_paint(x, y, count, rgb);
        }
    }
    const esp_err_t error = display_commit();
    display_test_frame_end();
    return error;
}

}  // namespace

esp_err_t webui_redraw_test() {
    const auto pattern = s_pattern.load();
    if (pattern != TestPattern::None) return paint_pattern(pattern);
    return ESP_OK;
}

const char* webui_test_pattern() {
    switch (s_pattern.load()) {
        case TestPattern::Identify: return "identify";
        case TestPattern::Grid: return "grid";
        case TestPattern::Red: return "red";
        case TestPattern::Green: return "green";
        case TestPattern::Blue: return "blue";
        case TestPattern::Gray: return "gray";
        default: return "";
    }
}

namespace webui {

esp_err_t send_test_state(httpd_req_t* req) {
    char buffer[kJsonSinkBuffer];
    JsonSink json = make_http_sink(req, buffer, sizeof(buffer));
    json.object_open();
    json.string("pattern", webui_test_pattern());
    json.object_close();
    return finish_http_sink(req, json);
}

esp_err_t handle_test(httpd_req_t* req) {
    size_t len = 0; char* body = read_json_body(req, &len); if (!body) return ESP_OK;
    cJSON* root = cJSON_ParseWithLength(body, len); free(body);
    bool known_keys = cJSON_IsObject(root);
    if (known_keys) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) known_keys &= strcmp(item->string, "release") == 0 || strcmp(item->string, "pattern") == 0;
    }
    const cJSON* release = root ? cJSON_GetObjectItemCaseSensitive(root, "release") : nullptr;
    const cJSON* pattern = root ? cJSON_GetObjectItemCaseSensitive(root, "pattern") : nullptr;
    if (!known_keys || (release && !cJSON_IsTrue(release)) ||
        (pattern && !cJSON_IsString(pattern)) || (!!release == !!pattern)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "Недопустимый тест");
    }
    const TestPattern selected = cJSON_IsString(pattern) ? pattern_value(pattern->valuestring) : TestPattern::None;
    const TestPattern previous = s_pattern.load();
    if (cJSON_IsTrue(release)) {
        cJSON_Delete(root);
        if (previous == TestPattern::None) return send_test_state(req);
        if (!show_active()) {
            const esp_err_t error = paint_pattern(TestPattern::None);
            if (error != ESP_OK) return send_json_result(req, error, "Не выключил тест");
        }
        s_pattern.store(TestPattern::None);
        display_external_drop();
        display_test_hold(show_active());
        show_pause_for_test(false);
        return send_test_state(req);
    }
    if (selected == TestPattern::None) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "Неизвестный тест-паттерн");
    }
    display_test_hold(true);
    show_pause_for_test(true);
    const esp_err_t error = paint_pattern(selected);
    cJSON_Delete(root);
    if (error != ESP_OK) {
        if (previous != TestPattern::None) paint_pattern(previous);
        else {
            display_test_hold(show_active());
            show_pause_for_test(false);
        }
        return send_json_result(req, error, "Не показал тест");
    }
    s_pattern.store(selected);
    return send_test_state(req);
}

}  // namespace webui
}  // namespace ledcast
