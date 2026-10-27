#include "linear.h"

#include <algorithm>
#include <atomic>
#include "display.h"

namespace ledcast {
namespace {
LineConfig s_configs[2]{};
std::atomic<uint8_t> s_active{0};
const LineConfig& live() { return s_configs[s_active.load(std::memory_order_acquire)]; }
}

esp_err_t linear_configure(const LineConfig& config) {
    if (config.module_count == 0 || config.module_count > kMaxLineModules ||
        config.module.width == 0 || config.module.height == 0) return ESP_ERR_INVALID_ARG;
    bool seen[kMaxLineModules]{};
    for (uint8_t i = 0; i < config.module_count; ++i) {
        const uint8_t physical = config.modules[i].physical_module;
        if (physical >= config.module_count || seen[physical]) return ESP_ERR_INVALID_ARG;
        seen[physical] = true;
    }
    const uint8_t next = s_active.load(std::memory_order_relaxed) ^ 1u;
    s_configs[next] = config;
    s_active.store(next, std::memory_order_release);
    return ESP_OK;
}

uint16_t linear_width() { return live().module.width * live().module_count; }
uint16_t linear_module_width() { return live().module.width; }
uint16_t linear_height() { return live().module.height; }

LineCoord linear_to_chain(uint16_t x, uint16_t y) {
    const auto& cfg = live();
    if (x >= cfg.module.width * cfg.module_count || y >= cfg.module.height) return {0,0,false};
    const uint8_t visual = x / cfg.module.width;
    uint16_t local_x = x % cfg.module.width;
    uint16_t local_y = y;
    const auto& module = cfg.modules[visual];
    if (module.rotate_180) { local_x = cfg.module.width - 1 - local_x; local_y = cfg.module.height - 1 - local_y; }
    if (module.mirror_x) local_x = cfg.module.width - 1 - local_x;
    if (module.mirror_y) local_y = cfg.module.height - 1 - local_y;
    const uint16_t slot = cfg.module_count - 1 - module.physical_module;
    return {static_cast<uint16_t>(slot * cfg.module.width + local_x), local_y, true};
}

void linear_paint(uint16_t x, uint16_t y, uint16_t count, const uint8_t* rgb) {
    const auto& cfg = live();
    const uint16_t width = cfg.module.width * cfg.module_count;
    if (rgb == nullptr || y >= cfg.module.height || x >= width) return;
    if (count > width - x) count = width - x;

    uint16_t painted = 0;
    while (painted < count) {
        const uint16_t source_x = x + painted;
        const uint8_t visual = source_x / cfg.module.width;
        const uint16_t local_x = source_x % cfg.module.width;
        const uint16_t run = std::min<uint16_t>(count - painted, cfg.module.width - local_x);
        const auto& module = cfg.modules[visual];
        const bool reverse_x = module.rotate_180 != module.mirror_x;
        const bool reverse_y = module.rotate_180 != module.mirror_y;
        const uint16_t slot = cfg.module_count - 1 - module.physical_module;
        const uint16_t mapped_x = reverse_x ? cfg.module.width - 1 - local_x : local_x;
        const uint16_t mapped_y = reverse_y ? cfg.module.height - 1 - y : y;
        display_write_run(slot * cfg.module.width + mapped_x, mapped_y, run,
                          reverse_x ? -1 : 1, rgb + painted * 3);
        painted += run;
    }
}

void linear_blackout() {
    const auto& cfg = live();
    for (uint16_t y = 0; y < cfg.module.height; ++y)
        display_clear_run(0, y, cfg.module.width * cfg.module_count, 1);
    display_commit();
}

}  // namespace ledcast
