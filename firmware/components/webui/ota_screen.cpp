// Экран загрузки прошивки: полоса заполняет строку слева направо и по ходу
// меняет цвет от красного к зелёному, по центру — «42%» во всю высоту строки.
// Человек у строки видит, что она жива и сколько осталось, не открывая веб.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "display.h"
#include "linear.h"
#include "show_font.h"
#include "webui_internal.h"

namespace ledcast::webui {
namespace {

constexpr uint8_t kMaxChars = 4;  // «100%»
// Уже пяти точек цифра не читается.
constexpr uint16_t kMinGlyphWidth = 5;

struct Line {
    uint32_t text[kMaxChars];
    uint8_t count = 0;
    uint16_t glyph_w = 0;
    uint16_t glyph_h = 0;
    uint16_t gap = 0;
    uint16_t left = 0;
    uint16_t top = 0;
};

uint8_t decode(const char* utf8, uint32_t* out) {
    const char* cursor = utf8;
    const char* end = utf8 + std::strlen(utf8);
    uint8_t count = 0;
    while (cursor < end && count < kMaxChars) out[count++] = show_font::utf8_next(cursor, end);
    return count;
}

// Буквы во всю отведённую высоту; ширина — по пропорции шрифта 5×7 или уже,
// чтобы строка влезла. false — не влезает и при самых узких буквах.
bool fit(Line& line, uint16_t width, uint16_t height, uint16_t top) {
    if (line.count == 0 || height < 7) return false;
    line.glyph_h = height;
    line.gap = height >= 14 ? 2 : 1;
    const uint16_t natural = static_cast<uint16_t>((height * 5u + 3u) / 7u);
    const uint16_t room = static_cast<uint16_t>((width + line.gap) / line.count);
    if (room <= line.gap) return false;
    line.glyph_w = natural < room - line.gap ? natural : static_cast<uint16_t>(room - line.gap);
    if (line.glyph_w < kMinGlyphWidth) return false;
    const uint16_t used = static_cast<uint16_t>(line.count * (line.glyph_w + line.gap) - line.gap);
    line.left = static_cast<uint16_t>((width - used) / 2u);
    line.top = top;
    return true;
}

bool ink(const Line& line, uint16_t x, uint16_t y) {
    if (y < line.top || y >= line.top + line.glyph_h || x < line.left) return false;
    const uint16_t cell = line.glyph_w + line.gap;
    const uint16_t index = (x - line.left) / cell;
    const uint16_t column = (x - line.left) % cell;
    if (index >= line.count || column >= line.glyph_w) return false;
    return show_font::pixel(show_font::glyph(line.text[index]), line.glyph_w, line.glyph_h, column,
                            y - line.top);
}

// Цвет полосы: тон от 0° (красный) до 120° (зелёный), неполная яркость, чтобы
// белый текст читался.
void bar_color(uint8_t percent, uint8_t rgb[3]) {
    constexpr uint32_t kValue = 150;
    const uint32_t hue = 120u * percent / 100u;
    if (hue < 60u) {
        rgb[0] = kValue;
        rgb[1] = static_cast<uint8_t>(kValue * hue / 60u);
    } else {
        rgb[0] = static_cast<uint8_t>(kValue * (120u - hue) / 60u);
        rgb[1] = kValue;
    }
    rgb[2] = 0;
}

}  // namespace

esp_err_t paint_ota_progress(uint8_t percent) {
    if (percent > 100) percent = 100;
    const uint16_t width = linear_width();
    const uint16_t height = linear_height();
    if (width == 0 || height == 0) return ESP_ERR_INVALID_STATE;

    char number[8];
    std::snprintf(number, sizeof(number), "%u%%", static_cast<unsigned>(percent));
    const uint16_t margin = height >= 14 ? 1 : 0;
    Line line{};
    line.count = decode(number, line.text);
    const bool text = fit(line, width, static_cast<uint16_t>(height - 2 * margin), margin);

    uint8_t bar[3];
    bar_color(percent, bar);
    // Незаполненная часть едва светится: видно, где у строки конец.
    constexpr uint8_t kTrack[3] = {10, 10, 10};
    const uint32_t filled = uint32_t(width) * percent / 100u;
    if (!display_test_frame_begin()) return ESP_ERR_TIMEOUT;
    constexpr uint16_t kChunk = 64;
    uint8_t rgb[kChunk * 3];
    for (uint16_t y = 0; y < height; ++y) {
        for (uint16_t x = 0; x < width; x += kChunk) {
            const uint16_t count = x + kChunk <= width ? kChunk : static_cast<uint16_t>(width - x);
            for (uint16_t i = 0; i < count; ++i) {
                const bool lit = text && ink(line, x + i, y);
                const uint8_t* fill = x + i < filled ? bar : kTrack;
                for (uint8_t c = 0; c < 3; ++c) rgb[i * 3 + c] = lit ? 255 : fill[c];
            }
            linear_paint(x, y, count, rgb);
        }
    }
    const esp_err_t error = display_commit();
    display_test_frame_end();
    return error;
}

}  // namespace ledcast::webui
