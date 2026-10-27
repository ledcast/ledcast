#pragma once

#include <cstdint>
#include <esp_err.h>

#include "power_cycles.h"

// Живучесть устройства: безопасный режим, подтверждение прошивки, кнопка,
// статусный светодиод, счётчики для интерфейса.
//
// Плата стоит внутри конструкции на площадке — до неё не дотянуться, поэтому
// прошивка обязана уметь подниматься сама.

namespace ledcast {

// Пины платы Huidu HD-WF2, распиновка подтверждена официальным board-variant.
inline constexpr int kPinStatusLed = 40;
inline constexpr int kPinButton = 17;  // 1 — отпущена, 0 — нажата
inline constexpr int kPinRtcSda = 41;
inline constexpr int kPinRtcScl = 42;

enum class BootMode : uint8_t {
    Normal,
    Safe,  // три неудачных загрузки подряд: сеть, веб и OTA без вывода на матрицу
};

enum class LedPattern : uint8_t {
    Booting,
    ApWaiting,
    StaConnecting,
    Running,
    StreamActive,
    Error,
    Identify,
};

enum class ButtonEvent : uint8_t {
    Hold5s,       // сбросить настройки сети
    Hold10s,      // заводской сброс
};

using ButtonHandler = void (*)(ButtonEvent event);

struct HealthStats {
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t largest_free_block;
    uint8_t boot_failures;
    BootMode boot_mode;
    char firmware_version[24];
    char reset_reason[24];
};

// Последняя паника: причина, адрес и стек вызовов (адреса разбирает
// addr2line по ledcast-node.elf той же сборки). Живёт до снятия питания.
struct PanicRecord {
    uint8_t core;
    uint8_t depth;
    uint32_t addr;
    uint32_t backtrace[12];
    char reason[96];
};
bool health_last_panic(PanicRecord& out);

// Вызывается первой в app_main: считает неудачные загрузки и решает режим.
BootMode health_init();

// Какое это быстрое включение питания подряд, см. power_cycles.h.
uint8_t health_power_cycles();
// Окно включения закрылось: следующее включение считается снова с первого.
void health_power_cycles_settle();

// Подтверждение, что сборка живая: до этого вызова загрузчик готов откатиться.
void health_mark_running();

esp_err_t health_button_start(ButtonHandler handler);
void health_led_set(LedPattern pattern);
HealthStats health_get_stats();


}  // namespace ledcast
