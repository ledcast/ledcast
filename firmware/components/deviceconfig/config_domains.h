#pragma once

#include <cstddef>
#include <cstdint>

#include "deviceconfig.h"

// Настройки лежат в NVS по доменам — каждый своим ключом, и каждый
// загружается, проверяется и откатывается к заводскому сам по себе. Правило
// смены схемы или негодное значение в одном домене не трогает остальные, а сеть
// и доступ к вебу не зависят ни от чего: плата внутри конструкции доступна
// только по Wi-Fi.
//
// Здесь только решения — без NVS и cJSON, чтобы они собирались на хосте под
// тесты. Чтение и запись — config_store.cpp.

namespace ledcast {

enum class ConfigDomain : uint8_t {
    Net,       // WifiConfig: сети, пароль и канал точки доступа, имя
    Security,  // SecurityConfig: вход в веб
    Line,      // LineConfig: тип модуля, их число и порядок
    Output,    // DisplayConfig: разрядность, шина, частота кадров, яркость
    Show,      // ShowConfig
    Stream,    // StreamConfig
};
inline constexpr ConfigDomain kConfigDomains[] = {
    ConfigDomain::Net, ConfigDomain::Security, ConfigDomain::Line,
    ConfigDomain::Output, ConfigDomain::Show, ConfigDomain::Stream,
};
inline constexpr size_t kConfigDomainCount = sizeof(kConfigDomains) / sizeof(kConfigDomains[0]);
inline constexpr size_t config_domain_index(ConfigDomain domain) { return static_cast<size_t>(domain); }

enum class DomainStatus : uint8_t {
    Loaded,      // прочитан и прошёл проверку
    Missing,     // в NVS нет — заводской
    Unreadable,  // не прочитался или не разобрался — заводской
    Imported,    // перенесён из старого общего документа
    Invalid,     // разобрался, но сам по себе негоден — заводской
    Conflict,    // сам по себе годен, но не сочетается с другим доменом — заводской
};

struct DomainLoad {
    DomainStatus status = DomainStatus::Missing;
    char reason[64]{};
};
using DomainLoads = DomainLoad[kConfigDomainCount];

esp_err_t config_validate_domain(ConfigDomain domain, const DeviceConfig& cfg, char* reason,
                                 size_t reason_len);
// Связь доменов: строка вместе с выводом должна влезть в DMA-пул.
esp_err_t config_validate_fit(const DeviceConfig& cfg, char* reason, size_t reason_len);
void config_reset_domain(ConfigDomain domain, DeviceConfig& cfg);

// Доводит домены, прочитанные по отдельности, до годной конфигурации. На входе
// домены со статусом Loaded и Imported уже разобраны в cfg, остальные заводские.
// Негодный домен откатывается к заводскому один. Если не сходится связка, первым
// уступает вывод, затем строка; сеть, доступ, показ и поток не трогаются.
void config_settle(DeviceConfig& cfg, DomainLoads& loads);

// Разовый перенос со старого общего документа: только сеть и доступ к вебу.
void config_import_legacy(const DeviceConfig& legacy, DeviceConfig& cfg, DomainLoads& loads);

}  // namespace ledcast
