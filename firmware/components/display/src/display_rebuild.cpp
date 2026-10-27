#include "display_rebuild.h"

namespace ledcast {

bool display_needs_rebuild(const ModuleType& before, uint16_t before_count,
                           const DisplayConfig& before_cfg, const ModuleType& after,
                           uint16_t after_count, const DisplayConfig& after_cfg) {
    if (before_count != after_count) return true;
    if (before.width != after.width || before.height != after.height) return true;
    if (before.scan_divisor != after.scan_divisor) return true;
    if (before.oe_active_low != after.oe_active_low) return true;
    if (before.data_active_high != after.data_active_high) return true;
    // Чип сдвигового регистра и дешифратор строк драйвер защёлкивает в панель
    // при старте и на живой панели не переучивает.
    if (before.shift_driver != after.shift_driver) return true;
    if (before.line_decoder != after.line_decoder) return true;
    if (before_cfg.bit_depth != after_cfg.bit_depth) return true;
    if (before_cfg.bus_hz != after_cfg.bus_hz) return true;
    // Число копий кадра двоит и буфер, и цепочку дескрипторов.
    if (before_cfg.buffers != after_cfg.buffers) return true;
    if (before_cfg.latch_blanking != after_cfg.latch_blanking) return true;
    // Частота кадров задаёт точку перелома BCM и длину добивки прохода.
    if (before_cfg.frame_rate != after_cfg.frame_rate) return true;
    if (before_cfg.clk_phase != after_cfg.clk_phase) return true;
    return false;
}

}  // namespace ledcast
