#include "deviceconfig.h"
#include "config_domains.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "../hub75-panel/src/bcm_memory.h"

namespace ledcast {
namespace {

esp_err_t fail(char* reason, size_t len, const char* format, ...) {
    if (reason != nullptr && len != 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(reason, len, format, args);
        va_end(args);
    }
    return ESP_ERR_INVALID_ARG;
}

bool valid_bus(uint32_t value) {
    for (uint32_t candidate : kBusFrequencies) if (candidate == value) return true;
    return false;
}

}  // namespace

uint8_t config_dma_overhead(ScanLayout layout) {
    switch (layout) {
        case ScanLayout::FourScan16: case ScanLayout::FourScan32:
        case ScanLayout::FourScan40: case ScanLayout::FourScan64:
        case ScanLayout::P10Base8: case ScanLayout::P10Base8Plain:
        case ScanLayout::P10Base16: case ScanLayout::Stripe:
        case ScanLayout::FlippedStripe: case ScanLayout::Checkered:
        case ScanLayout::Spiral: case ScanLayout::ZStripe:
        case ScanLayout::ZStripeUneven: case ScanLayout::Coreman: return 2;
        case ScanLayout::P10Quarter: case ScanLayout::Kaler2Scan: return 4;
        default: return 1;
    }
}

uint32_t config_dma_bytes_per_module(const ModuleType& module, uint8_t depth, uint8_t buffers) {
    const auto geometry = config_dma_geometry(module);
    return hub75_bcm::align4(uint32_t(geometry.width) * 2u * depth) * geometry.rows * buffers;
}

DmaGeometry config_dma_geometry(const ModuleType& module) {
    const auto layout = module.scan_layout;
    if (layout == ScanLayout::P10Quarter || layout == ScanLayout::Kaler2Scan)
        return {static_cast<uint16_t>(module.width * 4u), 4};
    const bool four = config_dma_overhead(layout) == 2 ||
        (layout == ScanLayout::Auto && module.scan_divisor && module.height / module.scan_divisor == 4);
    return {static_cast<uint16_t>(module.width * (four ? 2u : 1u)),
            static_cast<uint8_t>(module.height / (four ? 4u : 2u))};
}

uint32_t config_bcm_plane_weight(uint8_t depth, uint8_t transition, uint8_t plane) {
    (void)depth;
    return plane <= transition ? 1u : (1u << (plane - transition));
}

uint32_t config_bcm_extra_passes(uint8_t depth, uint8_t transition) {
    uint32_t total = 0;
    for (uint8_t plane = 0; plane < depth; ++plane) total += config_bcm_plane_weight(depth, transition, plane) - 1u;
    return total;
}

uint32_t config_dma_bytes(const LineConfig& line, const DisplayConfig& display, uint8_t depth) {
    const auto geometry = config_dma_geometry(line.module);
    const bool interleaved = hub75_bcm::kInterleavedRows &&
        line.module.line_decoder != LineDecoder::SM5266P && line.module.line_decoder != LineDecoder::SM5368;
    return hub75_bcm::dma_memory(geometry.width * line.module_count, geometry.rows, depth,
                                display.buffers, display.bus_hz, display.frame_rate,
                                interleaved).total_bytes;
}

uint8_t config_chain_capacity(const LineConfig& line, const DisplayConfig& display,
                              uint8_t depth, uint8_t buffers) {
    LineConfig probe = line;
    DisplayConfig output = display;
    output.buffers = buffers;
    for (uint8_t count = kMaxLineModules; count > 0; --count) {
        probe.module_count = count;
        if (config_dma_bytes(probe, output, depth) <= kDmaPoolBytes) return count;
    }
    return 0;
}

uint8_t config_max_bit_depth(const LineConfig& line, const DisplayConfig& display) {
    for (uint8_t depth = 8; depth >= 2; --depth) {
        if (config_chain_capacity(line, display, depth, display.buffers) >= line.module_count) return depth;
    }
    return 0;
}

namespace {

LineConfig default_line() {
    LineConfig line{};
    for (uint8_t i = 0; i < line.module_count; ++i) line.modules[i].physical_module = i;
    return line;
}

}  // namespace

DeviceConfig config_defaults() {
    DeviceConfig cfg{};
    cfg.line = default_line();
    return cfg;
}

namespace {

esp_err_t validate_line(const LineConfig& line, char* reason, size_t reason_len) {
    if (line.module_count == 0 || line.module_count > kMaxLineModules)
        return fail(reason, reason_len, "Line must contain 1..24 modules");
    if (line.module.width < 2 || line.module.width > 255 || line.module.height < 2 ||
        line.module.height > kMaxModuleHeight || line.module.height % 2 != 0)
        return fail(reason, reason_len, "Invalid module dimensions");
    // Перечисления из NVS могли пережить прошивку, где значений было больше.
    if (line.module.color_order > ColorOrder::BGR || line.module.scan_layout > kScanLayoutLast ||
        line.module.shift_driver > ShiftDriver::DP3246 || line.module.line_decoder > LineDecoder::SM5368)
        return fail(reason, reason_len, "Unknown module type");
    if (line.module.scan_divisor == 0 || line.module.height % line.module.scan_divisor != 0)
        return fail(reason, reason_len, "Invalid scan divisor");
    const uint16_t parallel_rows = line.module.height / line.module.scan_divisor;
    const auto layout = line.module.scan_layout;
    const bool standard_two = layout == ScanLayout::Auto || layout == ScanLayout::TwoScan;
    if (standard_two && parallel_rows != 2)
        return fail(reason, reason_len, "Standard layout requires a two-row scan");
    if ((layout == ScanLayout::FourScan16 && line.module.height != 16) ||
        (layout == ScanLayout::FourScan32 && line.module.height != 32) ||
        (layout == ScanLayout::FourScan40 && line.module.height != 40) ||
        (layout == ScanLayout::FourScan64 && line.module.height != 64) ||
        ((layout == ScanLayout::FourScan16 || layout == ScanLayout::FourScan32 ||
          layout == ScanLayout::FourScan40 || layout == ScanLayout::FourScan64) && parallel_rows != 4))
        return fail(reason, reason_len, "Four-row layout does not match module size or scan");
    const bool p10_layout = layout == ScanLayout::P10Base8 || layout == ScanLayout::P10Base8Plain ||
        layout == ScanLayout::P10Base16 || layout == ScanLayout::P10Quarter ||
        layout == ScanLayout::Checkered || layout == ScanLayout::Spiral || layout == ScanLayout::ZStripe;
    if (p10_layout && (line.module.width != 32 || line.module.height != 16 || line.module.scan_divisor != 4))
        return fail(reason, reason_len, "P10 layout requires a 32x16 module with 1/4 scan");
    bool seen[kMaxLineModules]{};
    for (uint8_t i = 0; i < line.module_count; ++i) {
        const uint8_t physical = line.modules[i].physical_module;
        if (physical >= line.module_count) return fail(reason, reason_len, "Module order is out of range");
        if (seen[physical]) return fail(reason, reason_len, "Module order is not a permutation");
        seen[physical] = true;
    }
    return ESP_OK;
}

esp_err_t validate_output(const DisplayConfig& display, char* reason, size_t reason_len) {
    if (display.bit_depth < 2 || display.bit_depth > 8 ||
        (display.buffers != 1 && display.buffers != 2) ||
        !valid_bus(display.bus_hz) ||
        display.latch_blanking < 1 || display.latch_blanking > 4 ||
        !config_frame_rate_valid(display.frame_rate))
        return fail(reason, reason_len, "Invalid display configuration");
    return ESP_OK;
}

esp_err_t validate_stream(const StreamConfig& stream, char* reason, size_t reason_len) {
    if (stream.port == 0 || stream.timeout_ms != 2000)
        return fail(reason, reason_len, "Invalid stream configuration");
    return ESP_OK;
}

esp_err_t validate_show(const ShowConfig& show, char* reason, size_t reason_len) {
    if (show.mode > ShowMode::ScrollRightToLeft || show.font_size_px < 7 ||
        show.font_size_px > kMaxModuleHeight ||
        show.speed_px_s < 1 ||
        show.speed_px_s > 240 || show.duration_s > 3600 ||
        (show.mode == ShowMode::Static && show.duration_s == 0) ||
        strnlen(show.text, sizeof(show.text)) >= sizeof(show.text))
        return fail(reason, reason_len, "Invalid autonomous show configuration");
    return ESP_OK;
}

esp_err_t validate_net(const WifiConfig& wifi, char* reason, size_t reason_len) {
    const size_t ap_password = strnlen(wifi.ap_password, sizeof(wifi.ap_password));
    if (ap_password < 8 || ap_password >= sizeof(wifi.ap_password))
        return fail(reason, reason_len, "AP password must contain 8..64 characters");
    if (wifi.ap_channel == 0 || wifi.ap_channel > 13 || wifi.profile_count > kWifiProfileLimit)
        return fail(reason, reason_len, "Invalid Wi-Fi configuration");
    for (uint8_t i = 0; i < wifi.profile_count; ++i) {
        if (wifi.profiles[i].ssid[0] == '\0') return fail(reason, reason_len, "Wi-Fi profile has empty SSID");
        for (uint8_t j = 0; j < i; ++j)
            if (strcmp(wifi.profiles[i].ssid, wifi.profiles[j].ssid) == 0)
                return fail(reason, reason_len, "Duplicate Wi-Fi profile");
    }
    return ESP_OK;
}

void mark(DomainLoads& loads, ConfigDomain domain, DomainStatus status, const char* reason) {
    DomainLoad& load = loads[config_domain_index(domain)];
    load.status = status;
    snprintf(load.reason, sizeof(load.reason), "%s", reason);
}

}  // namespace

esp_err_t config_validate_domain(ConfigDomain domain, const DeviceConfig& cfg, char* reason,
                                 size_t reason_len) {
    if (reason != nullptr && reason_len != 0) reason[0] = '\0';
    switch (domain) {
        case ConfigDomain::Net: return validate_net(cfg.wifi, reason, reason_len);
        // Пароль входа проверяет веб при вводе; хранимое значение годно любое.
        case ConfigDomain::Security: return ESP_OK;
        case ConfigDomain::Line: return validate_line(cfg.line, reason, reason_len);
        case ConfigDomain::Output: return validate_output(cfg.display, reason, reason_len);
        case ConfigDomain::Show: return validate_show(cfg.show, reason, reason_len);
        case ConfigDomain::Stream: return validate_stream(cfg.stream, reason, reason_len);
    }
    return fail(reason, reason_len, "Unknown configuration domain");
}

esp_err_t config_validate_fit(const DeviceConfig& cfg, char* reason, size_t reason_len) {
    if (config_max_bit_depth(cfg.line, cfg.display) < cfg.display.bit_depth)
        return fail(reason, reason_len, "Line does not fit DMA pool");
    return ESP_OK;
}

esp_err_t config_validate(const DeviceConfig& cfg, char* reason, size_t reason_len) {
    for (const ConfigDomain domain : kConfigDomains) {
        const esp_err_t err = config_validate_domain(domain, cfg, reason, reason_len);
        if (err != ESP_OK) return err;
    }
    return config_validate_fit(cfg, reason, reason_len);
}

// Без временной DeviceConfig: сброс зовётся при загрузке на стеке главной задачи.
void config_reset_domain(ConfigDomain domain, DeviceConfig& cfg) {
    switch (domain) {
        case ConfigDomain::Net: cfg.wifi = WifiConfig{}; break;
        case ConfigDomain::Security: cfg.security = SecurityConfig{}; break;
        case ConfigDomain::Line: cfg.line = default_line(); break;
        case ConfigDomain::Output: cfg.display = DisplayConfig{}; break;
        case ConfigDomain::Show: cfg.show = ShowConfig{}; break;
        case ConfigDomain::Stream: cfg.stream = StreamConfig{}; break;
    }
}

void config_settle(DeviceConfig& cfg, DomainLoads& loads) {
    char reason[sizeof(DomainLoad::reason)];
    for (const ConfigDomain domain : kConfigDomains) {
        const DomainStatus status = loads[config_domain_index(domain)].status;
        if (status != DomainStatus::Loaded && status != DomainStatus::Imported) continue;
        if (config_validate_domain(domain, cfg, reason, sizeof(reason)) == ESP_OK) continue;
        config_reset_domain(domain, cfg);
        mark(loads, domain, DomainStatus::Invalid, reason);
    }
    // Связка строки и вывода. Вывод уступает первым: его заново выставить
    // проще, чем раскладку модулей, — сначала заводской, затем с глубиной,
    // какая влезает. Строка сбрасывается, только если не влезает и 2 бита.
    if (config_validate_fit(cfg, reason, sizeof(reason)) == ESP_OK) return;
    const auto conflict = [&](ConfigDomain domain) {
        const DomainStatus status = loads[config_domain_index(domain)].status;
        if (status == DomainStatus::Loaded || status == DomainStatus::Missing)
            mark(loads, domain, DomainStatus::Conflict, reason);
    };
    conflict(ConfigDomain::Output);
    config_reset_domain(ConfigDomain::Output, cfg);
    if (config_validate_fit(cfg, nullptr, 0) == ESP_OK) return;
    if (const uint8_t depth = config_max_bit_depth(cfg.line, cfg.display); depth != 0) {
        cfg.display.bit_depth = depth;
        return;
    }
    conflict(ConfigDomain::Line);
    config_reset_domain(ConfigDomain::Line, cfg);
}

void config_import_legacy(const DeviceConfig& legacy, DeviceConfig& cfg, DomainLoads& loads) {
    cfg.wifi = legacy.wifi;
    cfg.security = legacy.security;
    mark(loads, ConfigDomain::Net, DomainStatus::Imported, "");
    mark(loads, ConfigDomain::Security, DomainStatus::Imported, "");
}

}  // namespace ledcast
