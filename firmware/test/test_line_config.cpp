#include "check.h"
#include "deviceconfig.h"

using namespace ledcast;

int main() {
    DeviceConfig cfg = config_defaults();
    CHECK(cfg.line.module_count == 1);
    CHECK(cfg.stream.enabled);
    CHECK(cfg.stream.timeout_ms == 2000);
    CHECK(cfg.display.frame_rate == 120);
    CHECK(cfg.display.bus_hz == 20000000);
    CHECK(cfg.show.font_size_px == 14);
    CHECK(cfg.show.mode == ShowMode::Static);
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.line.module_count = 24;
    for (uint8_t i = 0; i < 24; ++i) cfg.line.modules[i].physical_module = 23 - i;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    // Test the 24-module permutation with a configuration that fits the pool.
    cfg.display.bit_depth = 2;
    cfg.display.buffers = 1;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.line.modules[23].physical_module = 1;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    cfg = config_defaults(); cfg.line.module_count = 0;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    cfg = config_defaults(); cfg.wifi.profile_count = 11;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    cfg = config_defaults(); cfg.display.frame_rate = 59;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    cfg.display.frame_rate = 90;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    cfg.display.frame_rate = 121;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    // Длительность 0 — только у бегущей строки: фразы идут по очереди.
    cfg = config_defaults(); cfg.show.duration_s = 0;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    cfg.show.mode = ShowMode::ScrollRightToLeft;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.display.frame_rate = 30;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.display.frame_rate = 60;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.display.frame_rate = 120;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    // Шина только 16 или 20 МГц: остальные делители LCD_CAM нецелые.
    cfg = config_defaults(); cfg.display.bus_hz = 10000000;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    cfg.display.bus_hz = 16000000;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg = config_defaults(); cfg.show.font_size_px = 6;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);
    cfg = config_defaults(); cfg.show.font_size_px = 8;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.show.font_size_px = 16;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg = config_defaults(); cfg.show.font_size_px = 65;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);

    cfg = config_defaults();
    cfg.line.module.width = 64; cfg.line.module.height = 32;
    cfg.line.module.scan_divisor = 16; cfg.line.module.scan_layout = ScanLayout::Auto;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.line.module.scan_divisor = 8; cfg.line.module.scan_layout = ScanLayout::FourScan32;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.line.module.scan_layout = ScanLayout::FourScan16;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);

    cfg = config_defaults();
    cfg.line.module.width = 64; cfg.line.module.height = 64;
    cfg.line.module.scan_divisor = 32; cfg.line.module.scan_layout = ScanLayout::Auto;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.line.module.scan_divisor = 16; cfg.line.module.scan_layout = ScanLayout::FourScan64;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    cfg.line.module.scan_layout = ScanLayout::Spiral;
    CHECK(config_validate(cfg, nullptr, 0) == ESP_ERR_INVALID_ARG);

    CHECK(config_dma_overhead(ScanLayout::Spiral) == 2);
    CHECK(config_dma_overhead(ScanLayout::P10Quarter) == 4);
    return hosttest::failures ? 1 : 0;
}
