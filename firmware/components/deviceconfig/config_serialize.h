#pragma once

#include <cstddef>

#include "config_domains.h"
#include "deviceconfig.h"

// Внутренний контракт между сериализацией и хранилищем: домены уходят в NVS
// как есть, вместе с паролями. Наружу эти документы не отдаются — веб получает
// собственные ответы API без секретов.

namespace ledcast {

// Длина текста или -1, если не поместился в out_len.
int config_domain_to_json(ConfigDomain domain, const DeviceConfig& cfg, char* out, size_t out_len);
// Только разбор в поля домена: ESP_ERR_INVALID_ARG — не JSON-объект. Нет поля —
// остаётся прежнее значение, лишние поля не мешают; проверка — config_settle.
esp_err_t config_domain_from_json(ConfigDomain domain, const char* json, size_t len,
                                  DeviceConfig& out);
// Из старого общего документа разбирает только сеть и доступ к вебу.
esp_err_t config_legacy_from_json(const char* json, size_t len, DeviceConfig& out);

}  // namespace ledcast
