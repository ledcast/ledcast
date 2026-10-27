#include "power_cycle_screen.h"

#include <algorithm>
#include <cstring>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "display.h"
#include "health.h"
#include "linear.h"
#include "power_cycles.h"

namespace ledcast {
namespace {

constexpr uint8_t kWhite[3] = {255, 255, 255};
constexpr uint8_t kGreen[3] = {0, 255, 0};
constexpr uint8_t kBlack[3] = {0, 0, 0};
constexpr uint32_t kFrameMs = 50;

// Квадраты по центру панели, между ними полквадрата, под ними тонкая полоса.
struct Layout {
    uint16_t side, gap, left, top, bar_y, bar_height, span;
};

Layout layout() {
    const uint16_t width = linear_width(), height = linear_height();
    Layout l{};
    l.bar_height = std::max<uint16_t>(1, height / 16);
    l.side = std::max<uint16_t>(1, std::min<uint16_t>(height / 2, width / 4));
    l.gap = l.side / 2;
    l.span = static_cast<uint16_t>(power_cycles::kForStandalone * l.side +
                                   (power_cycles::kForStandalone - 1) * l.gap);
    l.left = static_cast<uint16_t>((width - std::min(width, l.span)) / 2);
    const uint16_t block = l.side + l.gap + l.bar_height;
    l.top = static_cast<uint16_t>((height - std::min(height, block)) / 2);
    l.bar_y = l.top + l.side + l.gap;
    return l;
}

bool lit(const Layout& l, uint8_t filled, uint16_t bar_px, int x, int y) {
    if (y >= l.bar_y && y < l.bar_y + l.bar_height) return x >= l.left && x < l.left + bar_px;
    if (y < l.top || y >= l.top + l.side) return false;
    for (uint8_t k = 0; k < power_cycles::kForStandalone; ++k) {
        const int x0 = l.left + k * (l.side + l.gap);
        if (x < x0 || x >= x0 + l.side) continue;
        if (k < filled) return true;
        // Пустой квадрат — рамка в точку.
        return y == l.top || y == l.top + l.side - 1 || x == x0 || x == x0 + l.side - 1;
    }
    return false;
}

void draw(const Layout& l, uint8_t filled, uint16_t bar_px, const uint8_t* colour) {
    if (!display_test_frame_begin()) return;
    const uint16_t width = linear_width(), height = linear_height();
    constexpr uint16_t kChunk = 64;
    uint8_t rgb[kChunk * 3];
    for (uint16_t y = 0; y < height; ++y) {
        for (uint16_t x = 0; x < width; x += kChunk) {
            const uint16_t amount = std::min<uint16_t>(kChunk, width - x);
            for (uint16_t i = 0; i < amount; ++i)
                std::memcpy(rgb + i * 3, lit(l, filled, bar_px, x + i, y) ? colour : kBlack, 3);
            linear_paint(x, y, amount, rgb);
        }
    }
    display_commit();
    display_test_frame_end();
}

struct Window {
    uint8_t cycles;
    bool output_ready;
};
Window s_window{};

void window_task(void*) {
    const Layout l = s_window.output_ready ? layout() : Layout{};
    bool drawn = false;
    for (int64_t now = esp_timer_get_time(); now < power_cycles::kWindowUs; now = esp_timer_get_time()) {
        // Шоу, поток и тест держат полотно сами — квадраты им не мешают.
        drawn = s_window.output_ready && !display_external_active();
        if (drawn) {
            const auto bar_px = static_cast<uint16_t>(l.span * (power_cycles::kWindowUs - now) /
                                                      power_cycles::kWindowUs);
            draw(l, s_window.cycles, bar_px, kWhite);
        }
        vTaskDelay(pdMS_TO_TICKS(kFrameMs));
    }
    if (drawn && !display_external_active() && display_test_frame_begin()) {
        linear_blackout();
        display_test_frame_end();
    }
    health_power_cycles_settle();
    vTaskDelete(nullptr);
}

}  // namespace

void power_cycle_window_start(uint8_t cycles, bool output_ready) {
    s_window = {cycles, output_ready};
    if (xTaskCreate(window_task, "power_cycles", 3072, nullptr, 3, nullptr) != pdPASS) {
        // Без задачи окно закрывается сразу: серия не должна копиться вечно.
        health_power_cycles_settle();
    }
}

void power_cycle_standalone_mark() {
    draw(layout(), power_cycles::kForStandalone, 0, kGreen);
}

}  // namespace ledcast
