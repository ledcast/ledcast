#pragma once

#include <cstdint>

// Быстрые включения питания подряд — вместо кнопки: плата стоит в корпусе, и
// на площадке, где её Wi-Fi известен, а человека с доступом к нему нет, иначе
// строку не вывести из сети. Включение, после которого строка не проработала
// окно, засчитывается следующему; третье подряд — автономный запуск: своя
// точка доступа и встроенное шоу.
namespace ledcast::power_cycles {

inline constexpr uint8_t kForStandalone = 3;
// Окно считается от загрузки: квадраты на экране видны его большую часть.
inline constexpr int64_t kWindowUs = 5000000;

// Какое это включение подряд: 1…kForStandalone; 0 — загрузка не от питания.
// stored — счёт прошлых включений, окно которых не закрылось.
constexpr uint8_t count(bool power_reset, uint8_t stored) {
    return power_reset ? static_cast<uint8_t>(stored % kForStandalone + 1) : 0;
}

// Что хранить до конца окна: автономный запуск закрывает серию сразу.
constexpr uint8_t stored_after(uint8_t count) { return count < kForStandalone ? count : 0; }

}  // namespace ledcast::power_cycles
