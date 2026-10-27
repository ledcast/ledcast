#include "check.h"
#include "display_rebuild.h"

using ledcast::ColorOrder;
using ledcast::DisplayConfig;
using ledcast::LineDecoder;
using ledcast::ModuleType;
using ledcast::ScanLayout;
using ledcast::ShiftDriver;

namespace {

constexpr uint16_t kModules = 22;

bool rebuild(const ModuleType& module, const DisplayConfig& cfg) {
    const ModuleType base{};
    const DisplayConfig base_cfg{};
    return ledcast::display_needs_rebuild(base, kModules, base_cfg, module, kModules, cfg);
}

void test_same_settings_need_nothing() {
    CHECK(!rebuild(ModuleType{}, DisplayConfig{}));
}

void test_buffer_memory_fields_rebuild() {
    DisplayConfig cfg{};
    cfg.buffers = static_cast<uint8_t>(cfg.buffers == 1 ? 2 : 1);
    CHECK(rebuild(ModuleType{}, cfg));

    DisplayConfig depth{};
    depth.bit_depth = static_cast<uint8_t>(depth.bit_depth + 1);
    CHECK(rebuild(ModuleType{}, depth));

    DisplayConfig bus{};
    bus.bus_hz = bus.bus_hz == 20000000 ? 16000000 : 20000000;
    CHECK(rebuild(ModuleType{}, bus));

    ModuleType longer{};
    const DisplayConfig base_cfg{};
    CHECK(ledcast::display_needs_rebuild(ModuleType{}, kModules, base_cfg, longer, kModules + 1,
                                       base_cfg));
}

void test_electrical_fields_rebuild() {
    DisplayConfig latch{};
    latch.latch_blanking = static_cast<uint8_t>(latch.latch_blanking + 1);
    CHECK(rebuild(ModuleType{}, latch));

    DisplayConfig phase{};
    phase.clk_phase = !phase.clk_phase;
    CHECK(rebuild(ModuleType{}, phase));

    DisplayConfig rate{};
    rate.frame_rate = static_cast<uint8_t>(rate.frame_rate - 10);
    CHECK(rebuild(ModuleType{}, rate));

    ModuleType oe{};
    oe.oe_active_low = !oe.oe_active_low;
    CHECK(rebuild(oe, DisplayConfig{}));

    ModuleType data{};
    data.data_active_high = !data.data_active_high;
    CHECK(rebuild(data, DisplayConfig{}));
}

// Чип сдвигового регистра и дешифратор строк драйвер защёлкивает в панель при
// старте: без пересборки настройка молча не применяется.
void test_panel_chips_rebuild() {
    ModuleType shift{};
    shift.shift_driver = ShiftDriver::FM6126A;
    CHECK(rebuild(shift, DisplayConfig{}));

    ModuleType decoder{};
    decoder.line_decoder = LineDecoder::SM5266P;
    CHECK(rebuild(decoder, DisplayConfig{}));
}

// Раскладка, порядок цветов и яркость применяются на живой панели.
void test_live_fields_need_no_rebuild() {
    ModuleType layout{};
    layout.scan_layout = ScanLayout::ZStripe;
    CHECK(!rebuild(layout, DisplayConfig{}));

    ModuleType order{};
    order.color_order = order.color_order == ColorOrder::RGB ? ColorOrder::GRB : ColorOrder::RGB;
    CHECK(!rebuild(order, DisplayConfig{}));

    DisplayConfig bright{};
    bright.brightness = static_cast<uint8_t>(bright.brightness ^ 0xFFu);
    CHECK(!rebuild(ModuleType{}, bright));
}

}  // namespace

int main() {
    test_same_settings_need_nothing();
    test_buffer_memory_fields_rebuild();
    test_electrical_fields_rebuild();
    test_panel_chips_rebuild();
    test_live_fields_need_no_rebuild();
    return hosttest::report("display_rebuild");
}
