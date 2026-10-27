#pragma once

#include <cstdint>

#include "deviceconfig.h"

namespace ledcast {

// Нужна ли полная пересборка драйвера при переходе к новым настройкам вывода.
//
// Раскладка точек, порядок цветов и яркость ложатся на живую панель: именно их
// подбирают на монтаже, и гасить ради них строку незачем. Пересборки требует всё,
// что задаёт размер и содержимое буфера DMA, и всё, что драйвер защёлкивает в
// панель при старте, — иначе настройка молча не применится.
bool display_needs_rebuild(const ModuleType& before, uint16_t before_count,
                           const DisplayConfig& before_cfg, const ModuleType& after,
                           uint16_t after_count, const DisplayConfig& after_cfg);

}  // namespace ledcast
