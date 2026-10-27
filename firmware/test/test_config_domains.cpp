#include "check.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "config_domains.h"

// Загрузка по доменам: негодный домен откатывается к заводскому один, связка
// строки и вывода уступает выводом, потом строкой, а сеть и доступ к вебу не
// трогает ничто, кроме их собственной негодности.

using namespace ledcast;

namespace {

DomainStatus status(const DomainLoads& loads, ConfigDomain domain) {
    return loads[config_domain_index(domain)].status;
}

void load_all(DomainLoads& loads) {
    for (DomainLoad& load : loads) load.status = DomainStatus::Loaded;
}

// То, что лежит во flash у строки на площадке: пять модулей в обратном
// порядке, бегущая строка, своя сеть и вход в веб.
DeviceConfig site() {
    DeviceConfig cfg = config_defaults();
    cfg.line.module_count = 5;
    for (uint8_t i = 0; i < 5; ++i) cfg.line.modules[i].physical_module = static_cast<uint8_t>(4 - i);
    cfg.display.bus_hz = 16000000;
    cfg.display.brightness = 40;
    cfg.show.enabled = true;
    cfg.show.speed_px_s = 71;
    cfg.stream.port = 5000;
    cfg.wifi.profile_count = 1;
    std::snprintf(cfg.wifi.profiles[0].ssid, sizeof(cfg.wifi.profiles[0].ssid), "site");
    std::snprintf(cfg.wifi.profiles[0].password, sizeof(cfg.wifi.profiles[0].password), "sitepass");
    std::snprintf(cfg.wifi.hostname, sizeof(cfg.wifi.hostname), "stage");
    cfg.wifi.ap_channel = 11;
    cfg.security.web_auth_enabled = true;
    std::snprintf(cfg.security.web_password, sizeof(cfg.security.web_password), "secret");
    return cfg;
}

bool net_is_site(const DeviceConfig& cfg) {
    return cfg.wifi.profile_count == 1 && std::strcmp(cfg.wifi.profiles[0].ssid, "site") == 0 &&
           std::strcmp(cfg.wifi.profiles[0].password, "sitepass") == 0 &&
           std::strcmp(cfg.wifi.hostname, "stage") == 0 && cfg.wifi.ap_channel == 11;
}

bool security_is_site(const DeviceConfig& cfg) {
    return cfg.security.web_auth_enabled && std::strcmp(cfg.security.web_password, "secret") == 0;
}

void test_all_missing() {
    DeviceConfig cfg = config_defaults();
    DomainLoads loads{};
    config_settle(cfg, loads);
    for (const ConfigDomain domain : kConfigDomains) CHECK(status(loads, domain) == DomainStatus::Missing);
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
}

void test_all_loaded_intact() {
    DeviceConfig cfg = site();
    DomainLoads loads{};
    load_all(loads);
    config_settle(cfg, loads);
    for (const ConfigDomain domain : kConfigDomains) CHECK(status(loads, domain) == DomainStatus::Loaded);
    CHECK(cfg.line.module_count == 5 && cfg.display.brightness == 40 && net_is_site(cfg));
}

// Случай с площадки: прошивка перестала принимать шину 10 МГц. Сбрасывается
// только вывод, сеть остаётся.
void test_invalid_output_keeps_net() {
    DeviceConfig cfg = site();
    cfg.display.bus_hz = 10000000;
    DomainLoads loads{};
    load_all(loads);
    config_settle(cfg, loads);
    CHECK(status(loads, ConfigDomain::Output) == DomainStatus::Invalid);
    CHECK(std::strcmp(loads[config_domain_index(ConfigDomain::Output)].reason,
                      "Invalid display configuration") == 0);
    CHECK(cfg.display.bus_hz == DisplayConfig{}.bus_hz);
    CHECK(cfg.display.brightness == DisplayConfig{}.brightness);
    CHECK(net_is_site(cfg));
    CHECK(security_is_site(cfg));
    CHECK(cfg.line.module_count == 5 && cfg.line.modules[0].physical_module == 4);
    CHECK(cfg.show.enabled && cfg.show.speed_px_s == 71);
    CHECK(cfg.stream.port == 5000);
    for (const ConfigDomain domain : {ConfigDomain::Net, ConfigDomain::Security, ConfigDomain::Line,
                                      ConfigDomain::Show, ConfigDomain::Stream})
        CHECK(status(loads, domain) == DomainStatus::Loaded);
    CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
}

// Каждый домен откатывается сам: негодная строка не уносит вывод, негодный
// показ — сеть, негодная сеть — доступ к вебу.
void test_each_domain_falls_back_alone() {
    {
        DeviceConfig cfg = site();
        cfg.line.modules[1].physical_module = 4;  // не перестановка
        DomainLoads loads{};
        load_all(loads);
        config_settle(cfg, loads);
        CHECK(status(loads, ConfigDomain::Line) == DomainStatus::Invalid);
        CHECK(status(loads, ConfigDomain::Output) == DomainStatus::Loaded);
        CHECK(cfg.line.module_count == 1);
        CHECK(cfg.display.bus_hz == 16000000 && cfg.display.brightness == 40);
        CHECK(net_is_site(cfg) && cfg.show.speed_px_s == 71);
    }
    {
        DeviceConfig cfg = site();
        cfg.line.module.shift_driver = static_cast<ShiftDriver>(200);  // значение из будущей прошивки
        DomainLoads loads{};
        load_all(loads);
        config_settle(cfg, loads);
        CHECK(status(loads, ConfigDomain::Line) == DomainStatus::Invalid);
        CHECK(cfg.line.module.shift_driver == ShiftDriver::Generic);
    }
    {
        DeviceConfig cfg = site();
        cfg.show.font_size_px = 3;
        DomainLoads loads{};
        load_all(loads);
        config_settle(cfg, loads);
        CHECK(status(loads, ConfigDomain::Show) == DomainStatus::Invalid);
        CHECK(!cfg.show.enabled);
        CHECK(net_is_site(cfg) && cfg.line.module_count == 5 && cfg.display.brightness == 40);
    }
    {
        DeviceConfig cfg = site();
        cfg.wifi.ap_channel = 0;
        DomainLoads loads{};
        load_all(loads);
        config_settle(cfg, loads);
        CHECK(status(loads, ConfigDomain::Net) == DomainStatus::Invalid);
        CHECK(cfg.wifi.profile_count == 0 && cfg.wifi.ap_channel == WifiConfig{}.ap_channel);
        CHECK(security_is_site(cfg));
        CHECK(cfg.line.module_count == 5 && cfg.show.speed_px_s == 71);
    }
    {
        // Нечитаемый домен уже заводской: проверка его не трогает.
        DeviceConfig cfg = site();
        cfg.stream = StreamConfig{};
        DomainLoads loads{};
        load_all(loads);
        loads[config_domain_index(ConfigDomain::Stream)].status = DomainStatus::Unreadable;
        config_settle(cfg, loads);
        CHECK(status(loads, ConfigDomain::Stream) == DomainStatus::Unreadable);
        CHECK(net_is_site(cfg) && cfg.line.module_count == 5);
    }
}

// Строка и вывод по отдельности годны, но вместе не влезают в DMA-пул.
// Первым уступает вывод — заводской, затем с меньшей глубиной; строка — только
// если не влезает и так. Сеть и прочее не участвуют.
void test_cross_domain_order() {
    {
        DeviceConfig cfg = site();
        cfg.line.module_count = 12;
        for (uint8_t i = 0; i < 12; ++i) cfg.line.modules[i].physical_module = i;
        CHECK(config_validate_fit(cfg, nullptr, 0) != ESP_OK);
        const uint8_t depth = config_max_bit_depth(cfg.line, DisplayConfig{});
        CHECK(depth >= 2 && depth < DisplayConfig{}.bit_depth);
        DomainLoads loads{};
        load_all(loads);
        config_settle(cfg, loads);
        CHECK(status(loads, ConfigDomain::Output) == DomainStatus::Conflict);
        CHECK(status(loads, ConfigDomain::Line) == DomainStatus::Loaded);
        CHECK(cfg.line.module_count == 12);
        CHECK(cfg.display.bit_depth == depth && cfg.display.bus_hz == DisplayConfig{}.bus_hz);
        CHECK(net_is_site(cfg) && security_is_site(cfg) && cfg.show.speed_px_s == 71);
        CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    }
    {
        DeviceConfig cfg = site();
        cfg.line.module.width = 64; cfg.line.module.height = 64;
        cfg.line.module.scan_divisor = 32; cfg.line.module.scan_layout = ScanLayout::Auto;
        cfg.line.module_count = 24;
        for (uint8_t i = 0; i < 24; ++i) cfg.line.modules[i].physical_module = i;
        CHECK(config_validate_domain(ConfigDomain::Line, cfg, nullptr, 0) == ESP_OK);
        CHECK(config_max_bit_depth(cfg.line, DisplayConfig{}) == 0);
        DomainLoads loads{};
        load_all(loads);
        config_settle(cfg, loads);
        CHECK(status(loads, ConfigDomain::Output) == DomainStatus::Conflict);
        CHECK(status(loads, ConfigDomain::Line) == DomainStatus::Conflict);
        CHECK(cfg.line.module_count == 1 && cfg.line.module.width == ModuleType{}.width);
        CHECK(net_is_site(cfg) && security_is_site(cfg) && cfg.stream.port == 5000);
        CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    }
}

// Первая загрузка после обновления: доменов ещё нет, есть старый общий
// документ. Из него берутся только сеть и доступ, всё прочее заводское.
void test_legacy_import() {
    {
        const DeviceConfig legacy = site();
        DeviceConfig cfg = config_defaults();
        DomainLoads loads{};
        config_import_legacy(legacy, cfg, loads);
        config_settle(cfg, loads);
        CHECK(status(loads, ConfigDomain::Net) == DomainStatus::Imported);
        CHECK(status(loads, ConfigDomain::Security) == DomainStatus::Imported);
        CHECK(net_is_site(cfg) && security_is_site(cfg));
        for (const ConfigDomain domain : {ConfigDomain::Line, ConfigDomain::Output,
                                          ConfigDomain::Show, ConfigDomain::Stream})
            CHECK(status(loads, domain) == DomainStatus::Missing);
        CHECK(cfg.line.module_count == 1 && cfg.display.brightness == DisplayConfig{}.brightness);
        CHECK(!cfg.show.enabled && cfg.stream.port == StreamConfig{}.port);
    }
    {
        // Старый документ со старой негодной шиной: вывод из него не берётся
        // вовсе, сеть переносится.
        DeviceConfig legacy = site();
        legacy.display.bus_hz = 10000000;
        DeviceConfig cfg = config_defaults();
        DomainLoads loads{};
        config_import_legacy(legacy, cfg, loads);
        config_settle(cfg, loads);
        CHECK(net_is_site(cfg) && cfg.display.bus_hz == DisplayConfig{}.bus_hz);
        CHECK(config_validate(cfg, nullptr, 0) == ESP_OK);
    }
    {
        // Перенесённое проверяется как любой домен: негодная сеть — заводская,
        // доступ всё равно переносится.
        DeviceConfig legacy = site();
        std::snprintf(legacy.wifi.ap_password, sizeof(legacy.wifi.ap_password), "short");
        DeviceConfig cfg = config_defaults();
        DomainLoads loads{};
        config_import_legacy(legacy, cfg, loads);
        config_settle(cfg, loads);
        CHECK(status(loads, ConfigDomain::Net) == DomainStatus::Invalid);
        CHECK(std::strcmp(cfg.wifi.ap_password, WifiConfig{}.ap_password) == 0);
        CHECK(status(loads, ConfigDomain::Security) == DomainStatus::Imported);
        CHECK(security_is_site(cfg));
    }
}

}  // namespace

int main() {
    test_all_missing();
    test_all_loaded_intact();
    test_invalid_output_keeps_net();
    test_each_domain_falls_back_alone();
    test_cross_domain_order();
    test_legacy_import();
    return hosttest::report("config_domains");
}
