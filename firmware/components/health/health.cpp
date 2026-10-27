#include "health.h"

#include <atomic>

#include <cstdio>
#include <cstring>

#include <driver/gpio.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "netconfig.h"

namespace ledcast {
namespace {

constexpr char kTag[] = "health";

// LEDCAST starts with a clean health history. Reusing the former product's
// namespace can strand a freshly flashed controller in safe mode because old
// crash counters survive an application-only update.
constexpr char kNvsNamespace[] = "ledcast_health";
constexpr char kNvsKeyBootFailures[] = "bootfail";
constexpr char kNvsKeyPowerCycles[] = "pwrcycle";
constexpr uint8_t kSafeModeThreshold = 3;

// Светодиод RUN подключён активным уровнем «единица» (проверено конфигом ESPHome
// для этой платы), кнопка S1 — активным нулём.
constexpr int kLedOnLevel = 1;
constexpr int kButtonPressedLevel = 0;

constexpr uint32_t kButtonPollMs = 10;
constexpr uint8_t kDebounceSamples = 3;  // 30 мс дребезга гасится с запасом
constexpr uint32_t kHold5Ms = 5000;
constexpr uint32_t kHold10Ms = 10000;

// Прошивка подтверждается только после честного аптайма: сборка, которая
// поднимается и падает под нагрузкой на десятой секунде, не должна успевать
// отменить откат загрузчика.
constexpr uint64_t kConfirmDelayUs = 45ULL * 1000000ULL;
constexpr uint64_t kConfirmRetryUs = 15ULL * 1000000ULL;

struct LedStep {
    uint16_t ms;
    bool on;
};

// Рисунки подобраны так, чтобы различаться на глаз без секундомера:
// длительность вспышки и длина паузы у соседних состояний отличаются кратно.
constexpr LedStep kStepsBooting[] = {{100, true}, {100, false}};
constexpr LedStep kStepsApWaiting[] = {{100, true}, {150, false}, {100, true}, {900, false}};
constexpr LedStep kStepsStaConnecting[] = {{400, true}, {400, false}};
constexpr LedStep kStepsRunning[] = {{60, true}, {1940, false}};
constexpr LedStep kStepsStream[] = {{60, true}, {240, false}};
constexpr LedStep kStepsError[] = {{120, true}, {120, false}, {120, true},
                                   {120, false}, {120, true}, {1200, false}};
constexpr LedStep kStepsIdentify[] = {{50, true}, {50, false}};

// Обратная связь на удержание кнопки: человек у строки должен видеть, что порог
// пройден, иначе он отпустит кнопку наугад.
constexpr LedStep kStepsHold5[] = {{1000, true}};
constexpr LedStep kStepsHold10[] = {{60, true}, {60, false}};

struct LedProgram {
    const LedStep* steps;
    size_t count;
};

LedProgram program_for(LedPattern pattern) {
    switch (pattern) {
        case LedPattern::Booting:
            return {kStepsBooting, sizeof(kStepsBooting) / sizeof(LedStep)};
        case LedPattern::ApWaiting:
            return {kStepsApWaiting, sizeof(kStepsApWaiting) / sizeof(LedStep)};
        case LedPattern::StaConnecting:
            return {kStepsStaConnecting, sizeof(kStepsStaConnecting) / sizeof(LedStep)};
        case LedPattern::Running:
            return {kStepsRunning, sizeof(kStepsRunning) / sizeof(LedStep)};
        case LedPattern::StreamActive:
            return {kStepsStream, sizeof(kStepsStream) / sizeof(LedStep)};
        case LedPattern::Error:
            return {kStepsError, sizeof(kStepsError) / sizeof(LedStep)};
        case LedPattern::Identify:
            return {kStepsIdentify, sizeof(kStepsIdentify) / sizeof(LedStep)};
    }
    return {kStepsError, sizeof(kStepsError) / sizeof(LedStep)};
}

enum class HoldHint : uint8_t { None, Reached5s, Reached10s };

struct State {
    BootMode boot_mode = BootMode::Normal;
    uint8_t boot_failures = 0;
    uint8_t power_cycles = 0;
    esp_reset_reason_t reset_reason = ESP_RST_UNKNOWN;

    volatile LedPattern led_pattern = LedPattern::Booting;
    volatile HoldHint hold_hint = HoldHint::None;
    TaskHandle_t led_task = nullptr;

    ButtonHandler button_handler = nullptr;
    TaskHandle_t button_task = nullptr;

    esp_timer_handle_t confirm_timer = nullptr;
    std::atomic<bool> confirm_due{false};

};

State g_state;

const char* reset_reason_text(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "питание";
        case ESP_RST_EXT: return "внешний сброс";
        case ESP_RST_SW: return "перезапуск";
        case ESP_RST_PANIC: return "паника";
        case ESP_RST_INT_WDT: return "int watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "сон";
        case ESP_RST_BROWNOUT: return "просадка питания";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "скачок питания";
        case ESP_RST_CPU_LOCKUP: return "зависание cpu";
        default: return "неизвестно";
    }
}

// Неудачной загрузкой считается только падение самой прошивки. Снятие питания,
// просадка и внешний сброс — обычная жизнь площадки: рубильник и бросок тока от
// всей цепочки модулей не должны загонять плату в безопасный режим.
bool reset_is_crash(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_CPU_LOCKUP:
            return true;
        default:
            return false;
    }
}

esp_err_t nvs_ready() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

uint8_t nvs_u8_load(const char* key) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }
    uint8_t value = 0;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) {
        value = 0;
    }
    nvs_close(handle);
    return value;
}

void nvs_u8_store(const char* key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(kTag, "не открылся NVS для счётчика %s", key);
        return;
    }
    nvs_set_u8(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

void led_write(bool on) {
    gpio_set_level(static_cast<gpio_num_t>(kPinStatusLed), on ? kLedOnLevel : 1 - kLedOnLevel);
}

void confirm_if_due();

void led_task(void*) {
    LedPattern active = g_state.led_pattern;
    HoldHint active_hint = g_state.hold_hint;
    LedProgram program = program_for(active);
    size_t step = 0;

    while (true) {
        confirm_if_due();
        const LedPattern wanted = g_state.led_pattern;
        const HoldHint hint = g_state.hold_hint;
        if (wanted != active || hint != active_hint) {
            active = wanted;
            active_hint = hint;
            switch (active_hint) {
                case HoldHint::Reached5s:
                    program = {kStepsHold5, sizeof(kStepsHold5) / sizeof(LedStep)};
                    break;
                case HoldHint::Reached10s:
                    program = {kStepsHold10, sizeof(kStepsHold10) / sizeof(LedStep)};
                    break;
                case HoldHint::None:
                    program = program_for(active);
                    break;
            }
            step = 0;
        }

        const LedStep& current = program.steps[step];
        led_write(current.on);

        // Шаг режем на кусочки, иначе смена рисунка ждала бы конца длинной паузы.
        uint16_t left = current.ms;
        while (left > 0 && g_state.led_pattern == active && g_state.hold_hint == active_hint) {
            const uint16_t slice = left > 20 ? 20 : left;
            vTaskDelay(pdMS_TO_TICKS(slice));
            left -= slice;
        }

        step = (step + 1) % program.count;
    }
}

void emit(ButtonEvent event) {
    if (g_state.button_handler != nullptr) {
        g_state.button_handler(event);
    }
}

void button_task(void*) {
    bool stable_pressed = false;
    bool last_sample = false;
    uint8_t same_count = 0;

    uint32_t press_start_ms = 0;
    HoldHint reached = HoldHint::None;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kButtonPollMs));
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

        const bool sample =
            gpio_get_level(static_cast<gpio_num_t>(kPinButton)) == kButtonPressedLevel;
        if (sample == last_sample) {
            if (same_count < kDebounceSamples) {
                ++same_count;
            }
        } else {
            last_sample = sample;
            same_count = 1;
        }

        if (same_count >= kDebounceSamples && sample != stable_pressed) {
            stable_pressed = sample;
            if (stable_pressed) {
                press_start_ms = now_ms;
                reached = HoldHint::None;
            } else {
                const uint32_t held = now_ms - press_start_ms;
                g_state.hold_hint = HoldHint::None;
                reached = HoldHint::None;

                // Долгие события отдаём по отпусканию: сработай сброс сети прямо
                // на пятой секунде — до десятой секунды было бы не дожать.
                if (held >= kHold10Ms) {
                    emit(ButtonEvent::Hold10s);
                } else if (held >= kHold5Ms) {
                    emit(ButtonEvent::Hold5s);
                }
            }
        }

        if (stable_pressed) {
            const uint32_t held = now_ms - press_start_ms;
            if (held >= kHold10Ms && reached != HoldHint::Reached10s) {
                reached = HoldHint::Reached10s;
                g_state.hold_hint = HoldHint::Reached10s;
            } else if (held >= kHold5Ms && reached == HoldHint::None) {
                reached = HoldHint::Reached5s;
                g_state.hold_hint = HoldHint::Reached5s;
            }
        }
    }
}

void confirm_timer_cb(void*) {
    g_state.confirm_due.store(true);
}

// Подтверждение — это запись в otadata и NVS. Отдельной задачи под него нет:
// она создаётся уже после буфера вывода и Wi-Fi, и памяти на неё может не
// хватить — а без подтверждения загрузчик вправе откатить рабочую сборку.
// Поэтому работу делает задача светодиода: она поднимается раньше всех и
// живёт вечно.
void confirm_if_due() {
    if (!g_state.confirm_due.exchange(false)) {
        return;
    }

    // Прошивка, в которую нельзя зайти по сети, рабочей не считается: без веба
    // и OTA плату внутри конструкции уже не починить.
    if (net_get_status().state == NetState::Down) {
        ESP_LOGW(kTag, "сеть не поднялась — подтверждение прошивки отложено");
        esp_timer_start_once(g_state.confirm_timer, kConfirmRetryUs);
        return;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (running != nullptr && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(kTag, "прошивка подтверждена: %s", esp_err_to_name(err));
    }

    if (g_state.boot_failures != 0) {
        nvs_u8_store(kNvsKeyBootFailures, 0);
        g_state.boot_failures = 0;
    }
    ESP_LOGI(kTag, "аптайм %llu с при поднятой сети — сборка признана рабочей",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000000));
}

}  // namespace

BootMode health_init() {
    ESP_ERROR_CHECK(nvs_ready());

    gpio_config_t led_cfg{};
    led_cfg.pin_bit_mask = 1ULL << kPinStatusLed;
    led_cfg.mode = GPIO_MODE_OUTPUT;
    led_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    led_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    led_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    led_write(false);

    g_state.reset_reason = esp_reset_reason();

    uint8_t failures = nvs_u8_load(kNvsKeyBootFailures);
    if (reset_is_crash(g_state.reset_reason)) {
        if (failures < 0xFF) {
            ++failures;
        }
        nvs_u8_store(kNvsKeyBootFailures, failures);
    } else if (g_state.reset_reason == ESP_RST_SW || g_state.reset_reason == ESP_RST_USB) {
        // Осознанный перезапуск из UI/OTA или физический доступ по USB —
        // заявка «я починил»: счётчик обнуляется и позволяет выйти из
        // безопасного режима при сервисе контроллера.
        if (failures != 0) {
            failures = 0;
            nvs_u8_store(kNvsKeyBootFailures, 0);
        }
    }
    // Питание, просадка и внешний сброс счётчик не трогают — ни вверх, ни вниз.

    // Просадка — то же снятие питания: блок питания модулей разряжается
    // медленно, и плата успевает сброситься по просадке, а не с нуля.
    const bool power_reset = g_state.reset_reason == ESP_RST_POWERON ||
                             g_state.reset_reason == ESP_RST_BROWNOUT;
    const uint8_t stored_cycles = nvs_u8_load(kNvsKeyPowerCycles);
    g_state.power_cycles = power_cycles::count(power_reset, stored_cycles);
    const uint8_t keep_cycles = power_cycles::stored_after(g_state.power_cycles);
    if (keep_cycles != stored_cycles) nvs_u8_store(kNvsKeyPowerCycles, keep_cycles);

    g_state.boot_failures = failures;
    g_state.boot_mode = failures >= kSafeModeThreshold ? BootMode::Safe : BootMode::Normal;

    // Ядро 0: мигание светодиодом ничего не ждёт и не должно шевелить ядро 1,
    // где идёт перезапись битпланов матрицы.
    if (g_state.led_task == nullptr) {
        xTaskCreatePinnedToCore(led_task, "health_led", 4096, nullptr, 2, &g_state.led_task, 0);
    }

    ESP_LOGI(kTag, "загрузка #%u, причина сброса: %s, режим %s", failures,
             reset_reason_text(g_state.reset_reason),
             g_state.boot_mode == BootMode::Safe ? "безопасный" : "обычный");

    if (g_state.power_cycles != 0) {
        ESP_LOGI(kTag, "включение питания %u из %u подряд", g_state.power_cycles,
                 power_cycles::kForStandalone);
    }

    return g_state.boot_mode;
}

uint8_t health_power_cycles() { return g_state.power_cycles; }

void health_power_cycles_settle() {
    if (power_cycles::stored_after(g_state.power_cycles) != 0) nvs_u8_store(kNvsKeyPowerCycles, 0);
}

void health_mark_running() {
    // В безопасном режиме образ не подтверждаем и счётчик не трогаем: сборка
    // выжила только потому, что матрицу не поднимали, — это не доказательство.
    // Выход из безопасного режима — осознанный перезапуск (ESP_RST_SW).
    if (g_state.boot_mode == BootMode::Safe) {
        ESP_LOGW(kTag, "безопасный режим: прошивка не подтверждается, откат остаётся возможным");
        return;
    }

    if (g_state.confirm_timer == nullptr) {
        esp_timer_create_args_t args{};
        args.callback = confirm_timer_cb;
        args.name = "health_confirm";
        const esp_err_t err = esp_timer_create(&args, &g_state.confirm_timer);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "не создался таймер подтверждения: %s", esp_err_to_name(err));
            return;
        }
    }

    esp_timer_stop(g_state.confirm_timer);
    esp_timer_start_once(g_state.confirm_timer, kConfirmDelayUs);
}

esp_err_t health_button_start(ButtonHandler handler) {
    if (handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    g_state.button_handler = handler;
    if (g_state.button_task != nullptr) {
        return ESP_OK;
    }

    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << kPinButton;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    // Опрос, а не прерывание: обработчики событий трогают конфиг и сцену,
    // из ISR такое делать нельзя, а очередь ради одной кнопки — лишняя сущность.
    // Стек с запасом: обработчик кладёт на него копию конфигурации (~1.3 КБ) и
    // уходит в config_set со всеми реконфигурациями.
    // Ядро 1 намеренно: кнопка — спасательный люк заводского сброса. На ядре 1
    // живёт только рендер (приоритет 5), который спит 15 мс из 16, поэтому опрос
    // получит время даже когда ядро 0 забито сетью. Оставь её на ядре 0 — при
    // заторе там пропадёт и веб, и кнопка, и чинить плату придётся по USB.
    if (xTaskCreatePinnedToCore(button_task, "health_btn", 8192, nullptr, 3, &g_state.button_task,
                                1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void health_led_set(LedPattern pattern) { g_state.led_pattern = pattern; }

HealthStats health_get_stats() {
    HealthStats stats{};
    stats.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    stats.free_heap = esp_get_free_heap_size();
    stats.min_free_heap = esp_get_minimum_free_heap_size();
    // Именно непрерывный блок: DMA-буферу матрицы нужен один кусок, а не сумма.
    stats.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    stats.boot_failures = g_state.boot_failures;
    stats.boot_mode = g_state.boot_mode;

    const esp_app_desc_t* desc = esp_app_get_description();
    snprintf(stats.firmware_version, sizeof(stats.firmware_version), "%.23s",
             desc != nullptr ? desc->version : "?");
    snprintf(stats.reset_reason, sizeof(stats.reset_reason), "%s",
             reset_reason_text(g_state.reset_reason));
    return stats;
}

}  // namespace ledcast
