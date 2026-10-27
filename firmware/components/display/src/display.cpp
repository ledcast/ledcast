#include "display.h"

#include <atomic>

#include <driver/pulse_cnt.h>
#include <esp_attr.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "deviceconfig.h"
#include "display_rebuild.h"
#include "framestats.h"
#include "hd_wf2_pins.h"
#include "hub75_backend.h"

namespace ledcast {
namespace {

constexpr char kTag[] = "display";

// Окно замера частоты обновления: короче — шум округления, длиннее — статистика
// в вебе отстаёт от подкрутки настроек на монтаже.
constexpr int64_t kTachWindowUs = 250000;

// Счётчик импульсов 16-битный, поэтому переполнение накапливается в драйвере
// по сторожевой точке — иначе замер сломается через несколько минут.
constexpr int kTachHighLimit = 10000;

// Импульс адресной линии длится доли миллисекунды; всё короче микросекунды —
// не кадр, а помеха.
constexpr uint32_t kTachGlitchNs = 1000;

SemaphoreHandle_t s_lock = nullptr;
SemaphoreHandle_t s_frame_lock = nullptr;
bool s_started = false;

// Дедлайн захвата полотна внешним источником. Читается рендер-задачей на другом
// ядре каждый кадр, поэтому atomic.
std::atomic<int64_t> s_external_until{0};

// Что сейчас реально стоит на панели: нужно, чтобы отличить смену раскладки от
// смены геометрии и не пересобирать драйвер там, где это не требуется.
ModuleType s_module{};
DisplayConfig s_cfg{};
uint16_t s_module_count = 0;

bool light_change_only(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg) {
    return !display_needs_rebuild(s_module, s_module_count, s_cfg, module, module_count, cfg);
}

std::atomic<bool> s_ready{false};
uint16_t s_chain_width = 0;
uint16_t s_chain_height = 0;

hub75_backend::Info s_info{};

// Заполненность кадра. Копим сумму (r + g + b) прямо при записи пикселей —
// сложение вместо деления в горячем пути, делим один раз в display_commit.
// Кадр, в который никто не писал, не показывается, и оценка тока остаётся
// прежней — за это отвечает признак «в кадр писали».
uint32_t s_duty_accum = 0;
bool s_frame_touched = false;

// Делитель суммы (r + g + b) до доли максимума: точки кадра на три канала по 255.
uint32_t s_duty_divisor = 1;
std::atomic<uint32_t> s_duty_q16{0};
std::atomic<bool> s_test_active{false};

// Замер закрытия кадра: копим окно, наружу отдаём готовое. Живёт под s_lock —
// и пишется, и читается только с ним.
TimingWindow s_commit{kTachWindowUs, kCommitBudgetUs};

// Когда в последний раз жаловались на несостоявшееся ожидание обмена.
int64_t s_flip_fail_logged = 0;

// Кадры, реально доехавшие до панели: считаются после всех проверок и после
// обмена копиями буфера. Тоже под s_lock.
uint32_t s_frames_shown = 0;
uint32_t s_flip_failures = 0;
RateWindow s_shown_rate{1000000};

pcnt_unit_handle_t s_tach_unit = nullptr;
pcnt_channel_handle_t s_tach_chan = nullptr;
int s_tach_last_count = 0;
int64_t s_tach_last_us = 0;
std::atomic<uint32_t> s_refresh_hz{0};

// Тахометр кадров.
//
// Драйвер не отдаёт наружу ни счётчика кадров, ни прерывания по концу цепочки
// дескрипторов, а расчётная частота обновления как приёмочный критерий не
// годится. Считаем фронты адресной линии аппаратным PCNT и делим на число
// фронтов в полном BCM-периоде, вычисленное из фактического DMA schedule.
//
// Вход подключается через матрицу GPIO поверх уже настроенного выхода LCD_CAM:
// pcnt_new_channel трогает только input enable и входную маршрутизацию, вывод
// на панель остаётся нетронутым.
esp_err_t tach_start(int8_t gpio) {
    if (gpio < 0) {
        ESP_LOGW(kTag, "адресных линий меньше двух, частота обновления не измеряется");
        return ESP_OK;
    }

    pcnt_unit_config_t unit_config{};
    unit_config.low_limit = -1;
    unit_config.high_limit = kTachHighLimit;
    unit_config.flags.accum_count = 1;
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_config, &s_tach_unit), kTag, "нет свободного PCNT");

    pcnt_glitch_filter_config_t filter_config{};
    filter_config.max_glitch_ns = kTachGlitchNs;
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(s_tach_unit, &filter_config), kTag, "фильтр PCNT не принят");

    pcnt_chan_config_t chan_config{};
    chan_config.edge_gpio_num = gpio;
    chan_config.level_gpio_num = -1;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_tach_unit, &chan_config, &s_tach_chan), kTag, "канал PCNT не создан");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(s_tach_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                     PCNT_CHANNEL_EDGE_ACTION_HOLD),
                        kTag, "фронт PCNT не настроен");
    ESP_RETURN_ON_ERROR(
        pcnt_channel_set_level_action(s_tach_chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP),
        kTag, "уровень PCNT не настроен");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(s_tach_unit, kTachHighLimit), kTag, "сторожевая точка не принята");
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_tach_unit), kTag, "PCNT не включился");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_tach_unit), kTag, "PCNT не сбросился");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(s_tach_unit), kTag, "PCNT не запустился");

    s_tach_last_count = 0;
    s_tach_last_us = esp_timer_get_time();
    s_refresh_hz.store(0, std::memory_order_relaxed);
    ESP_LOGI(kTag, "частота обновления снимается с GPIO%d", static_cast<int>(gpio));
    return ESP_OK;
}

void tach_stop() {
    if (s_tach_unit == nullptr) {
        return;
    }
    pcnt_unit_stop(s_tach_unit);
    pcnt_unit_disable(s_tach_unit);
    if (s_tach_chan != nullptr) {
        pcnt_del_channel(s_tach_chan);
        s_tach_chan = nullptr;
    }
    pcnt_del_unit(s_tach_unit);
    s_tach_unit = nullptr;
    s_refresh_hz.store(0, std::memory_order_relaxed);
}

// Вызывается под s_lock из display_get_stats.
uint32_t tach_sample() {
    const uint32_t cached = s_refresh_hz.load(std::memory_order_relaxed);
    if (s_tach_unit == nullptr) {
        return 0;
    }

    const int64_t now = esp_timer_get_time();
    const int64_t elapsed = now - s_tach_last_us;
    if (elapsed < kTachWindowUs) {
        return cached;
    }

    int count = 0;
    if (pcnt_unit_get_count(s_tach_unit, &count) != ESP_OK) {
        return cached;
    }

    const int32_t pulses = count - s_tach_last_count;
    s_tach_last_count = count;
    s_tach_last_us = now;
    if (pulses < 0) {
        return cached;
    }

    const int64_t period = elapsed * s_info.addr_rising_edges_per_refresh;
    const uint32_t hz = period > 0
        ? static_cast<uint32_t>((static_cast<int64_t>(pulses) * 1000000 + period / 2) / period) : 0;
    s_refresh_hz.store(hz, std::memory_order_relaxed);
    return hz;
}

esp_err_t start_locked(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg) {
    hub75_backend::Info info{};
    ESP_RETURN_ON_ERROR(hub75_backend::start(module, module_count, cfg, info), kTag, "драйвер HUB75 не стартовал");

    s_info = info;
    s_chain_width = static_cast<uint16_t>(module.width * module_count);
    s_module = module;
    s_cfg = cfg;
    s_module_count = module_count;
    s_chain_height = module.height;
    s_duty_divisor = static_cast<uint32_t>(s_chain_width) * s_chain_height * 3u * 255u;
    s_duty_accum = 0;
    s_frame_touched = false;
    s_duty_q16.store(0, std::memory_order_relaxed);
    const int64_t started_us = esp_timer_get_time();
    s_commit.reset(started_us);
    s_shown_rate.reset(started_us);
    s_frames_shown = 0;
    s_flip_failures = 0;

    const esp_err_t tach_err = tach_start(info.addr_msb_gpio);
    if (tach_err != ESP_OK) {
        // Картинка важнее счётчика: панель уже светится, а без тахометра
        // статистика просто отдаёт ноль.
        ESP_LOGE(kTag, "тахометр кадров не поднялся: %s", esp_err_to_name(tach_err));
        tach_stop();
    }

    if (info.bit_depth != cfg.bit_depth) {
        ESP_LOGW(kTag, "запрошено %u бит на канал, драйвер даёт %u", static_cast<unsigned>(cfg.bit_depth),
                 static_cast<unsigned>(info.bit_depth));
    }
    ESP_LOGI(kTag, "цепочка %ux%u, %u модулей, %u бит, %u копий кадра, %u Гц шина, битпланы %u КБ",
             static_cast<unsigned>(s_chain_width), static_cast<unsigned>(s_chain_height),
             static_cast<unsigned>(module_count), static_cast<unsigned>(info.bit_depth),
             static_cast<unsigned>(cfg.buffers), static_cast<unsigned>(cfg.bus_hz),
             static_cast<unsigned>(info.dma_bytes / 1024));

    s_ready.store(true, std::memory_order_release);
    return ESP_OK;
}

// Закрыть кадр для записи и дождаться, пока рендер-задача пройдёт границу
// кадра. Зовётся строго без s_lock: рендер-задача берёт тот же замок на каждый
// прогон точек, и, пока замок занят, до закрытия кадра она не доходит — счётчик
// кадров стоит, и ожидание истекает всегда, когда задача успела встать в
// очередь за замком.
bool quiesce_render() {
    s_ready.store(false, std::memory_order_release);
    // Все писатели проверяют s_ready, а сам доступ к полотну защищён s_lock.
    // После снятия флага краткий захват замка гарантирует, что уже начатая
    // запись закончена.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xSemaphoreGive(s_lock);
    return true;
}

// Снос драйвера. Записи в кадр к этому моменту уже закрыты quiesce_render.
void stop_locked() {
    s_ready.store(false, std::memory_order_release);
    tach_stop();
    hub75_backend::stop();
    s_info = hub75_backend::Info{};
    s_chain_width = 0;
    s_chain_height = 0;
}

}  // namespace

esp_err_t display_init(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg) {
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_NO_MEM, kTag, "нет памяти под мьютекс");
    }
    if (s_frame_lock == nullptr) {
        s_frame_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_frame_lock != nullptr, ESP_ERR_NO_MEM, kTag,
                            "нет памяти под мьютекс кадра");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_started) {
        err = start_locked(module, module_count, cfg);
        s_started = err == ESP_OK;
    } else {
        ESP_LOGE(kTag, "display_init повторно; для смены настроек есть display_reconfigure");
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool display_config_begin() {
    return s_frame_lock != nullptr && xSemaphoreTake(s_frame_lock, portMAX_DELAY) == pdTRUE;
}

void display_config_end() {
    xSemaphoreGive(s_frame_lock);
}

esp_err_t display_reconfigure(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg) {
    // Хватает того, что display_init уже пытались вызвать: после неудачного
    // старта драйвера пересобрать его должно быть можно, иначе строку не поднять
    // без перезагрузки.
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_INVALID_STATE, kTag, "display_init ещё не вызывался");

    // Негодный конфиг не должен гасить работающую панель, поэтому проверка идёт
    // до остановки драйвера.
    ESP_RETURN_ON_ERROR(hub75_backend::validate(module, module_count, cfg), kTag, "новые настройки не приняты");

    // Раскладка точек и порядок цветов буфера DMA не касаются — их применяем
    // на живой панели. Пересобирать драйвер ради них нельзя: библиотека не
    // отдаёт канал DMA обратно, и через несколько пересборок панель уже не
    // поднимется. Подбор раскладки под конкретный модуль идёт именно так.
    if (s_started && light_change_only(module, module_count, cfg)) {
        // Буфера DMA раскладка не касается, а таблицу пересчёта координат
        // пересобирает целиком: старая освобождается, новая выделяется заново.
        // По этой же таблице идёт запись точек, и поточечный путь замка не
        // берёт — значит закрывать запись надо и здесь, по той же границе кадра,
        // что и при пересборке драйвера. Замок один этого не дал бы: писателей у
        // кадра четыре, а замок берут только трое.
        if (s_ready.load(std::memory_order_acquire) && !quiesce_render()) {
            ESP_LOGE(kTag, "рендер-задача не отдала кадр, раскладку не трогаем");
            return ESP_ERR_TIMEOUT;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        hub75_backend::set_color_order(module.color_order);
        const esp_err_t err = hub75_backend::set_scan_layout(module, module_count);
        if (err == ESP_OK) {
            s_module = module;
            s_cfg = cfg;
            hub75_backend::set_brightness(cfg.brightness);
        }
        // Запись открывается и после отказа: несошедшаяся раскладка оставляет
        // таблицу либо прежней, либо снятой вовсе, и ни в том, ни в другом
        // случае писать некуда — драйвер при этом остаётся рабочим.
        s_ready.store(true, std::memory_order_release);
        xSemaphoreGive(s_lock);
        ESP_LOGI(kTag, "раскладка и цвета применены без пересборки драйвера");
        return err;
    }

    // Ожидание границы кадра идёт до захвата s_lock и потому не под ним:
    // рендер-задача берёт этот же замок на каждый прогон точек.
    if (s_ready.load(std::memory_order_acquire) && !quiesce_render()) {
        ESP_LOGE(kTag, "рендер-задача не отдала кадр, драйвер не трогаем");
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_started) {
        stop_locked();
        s_started = false;
    }
    const esp_err_t err = start_locked(module, module_count, cfg);
    s_started = err == ESP_OK;
    xSemaphoreGive(s_lock);
    return err;
}

IRAM_ATTR void display_write_run(uint16_t x, uint16_t y, uint16_t count, int8_t step, const uint8_t* rgb) {
    if (s_lock == nullptr || rgb == nullptr || count == 0) {
        return;
    }
    if (!s_ready.load(std::memory_order_acquire) || y >= s_chain_height) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Второй раз уже под замком: между проверкой и захватом драйвер могли снести.
    if (s_ready.load(std::memory_order_acquire)) {
        // Ток оцениваем по сумме канала: точная сумма прогона стоила бы столько
        // же, сколько сам вывод, поэтому берём середину — для оценки нагрузки
        // блока питания этого достаточно.
        uint32_t sum = 0;
        for (uint16_t i = 0; i < count; ++i) {
            sum += static_cast<uint32_t>(rgb[i * 3]) + rgb[i * 3 + 1] + rgb[i * 3 + 2];
        }
        s_duty_accum += sum;
        s_frame_touched = true;
        hub75_backend::write_run(x, y, count, step, rgb);
    }
    xSemaphoreGive(s_lock);
}

IRAM_ATTR void display_clear_run(uint16_t x, uint16_t y, uint16_t count, int8_t step) {
    if (s_lock == nullptr || count == 0) {
        return;
    }
    if (!s_ready.load(std::memory_order_acquire) || y >= s_chain_height) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ready.load(std::memory_order_acquire)) {
        // Чёрный фон тока не потребляет, но это всё равно полноценная запись
        // кадра. Без флага display_commit() считал blackout пустым и оставлял в
        // DMA-буфере последний тестовый рисунок.
        s_frame_touched = true;
        hub75_backend::clear_run(x, y, count, step);
    }
    xSemaphoreGive(s_lock);
}

void display_external_hold(int64_t until_us) {
    s_external_until.store(until_us, std::memory_order_release);
}

bool display_external_active() {
    if (s_test_active.load(std::memory_order_acquire)) {
        return true;
    }
    const int64_t until = s_external_until.load(std::memory_order_acquire);
    return until != 0 && esp_timer_get_time() < until;
}

void display_external_drop() {
    s_external_until.store(0, std::memory_order_release);
}

void display_test_hold(bool active) {
    s_test_active.store(active, std::memory_order_release);
}

bool display_test_active() {
    return s_test_active.load(std::memory_order_acquire);
}

namespace {
// All complete-frame producers acquire the same writable DMA generation.
// Source selection happens before acquiring it; acquiring a buffer never
// changes the selected source as a side effect.
bool begin_complete_frame(bool local) {
    if (s_frame_lock == nullptr || s_lock == nullptr ||
        !s_ready.load(std::memory_order_acquire) ||
        (!local && display_test_active())) return false;
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    // The test may have started while this task waited behind another frame.
    if (!s_ready.load(std::memory_order_acquire) || (!local && display_test_active())) {
        xSemaphoreGive(s_frame_lock);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool resolved = false;
    const esp_err_t prepare_err = hub75_backend::prepare_frame(resolved);
    if (resolved) {
        ++s_frames_shown;
        s_shown_rate.tick(esp_timer_get_time());
    }
    xSemaphoreGive(s_lock);
    if (prepare_err != ESP_OK) {
        xSemaphoreGive(s_frame_lock);
        return false;
    }
    return true;
}
}  // namespace

bool display_external_frame_begin() { return begin_complete_frame(false); }

void display_external_frame_end() {
    if (s_frame_lock != nullptr) xSemaphoreGive(s_frame_lock);
}

bool display_test_frame_begin() {
    return begin_complete_frame(true);
}

void display_test_frame_end() {
    if (s_frame_lock != nullptr) xSemaphoreGive(s_frame_lock);
}

namespace {

// Заполненность кадра в доле от максимума, 16.16 — по ней оценивается ток.
void publish_duty(uint32_t accum) {
    uint32_t duty_q16 = static_cast<uint32_t>((static_cast<uint64_t>(accum) << 16) / s_duty_divisor);
    // Пиксель, перерисованный за кадр дважды, попадает в сумму дважды.
    if (duty_q16 > 65536) {
        duty_q16 = 65536;
    }
    s_duty_q16.store(duty_q16, std::memory_order_relaxed);
}

}  // namespace

esp_err_t display_commit() {
    if (s_lock == nullptr || !s_ready.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    // Замок тот же, что у display_write_run: обмен копиями не должен разрезать
    // чужой прогон точек.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready.load(std::memory_order_acquire)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_frame_touched) {
        // В кадр никто не писал: панель светит тем, что на ней уже есть, и
        // оценка тока остаётся прежней.
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    const uint32_t accum = s_duty_accum;
    s_duty_accum = 0;
    s_frame_touched = false;

    bool shown = true;
    esp_err_t result = ESP_OK;
    if (s_cfg.buffers > 1) {
        const int64_t started = esp_timer_get_time();
        const esp_err_t flip_err = hub75_backend::flip();
        const int64_t finished = esp_timer_get_time();
        // Замер — сколько закрытие кадра держит замок, вместе с ожиданием
        // границы прохода.
        s_commit.add(static_cast<uint32_t>(finished - started), finished);
        if (flip_err != ESP_OK && finished - s_flip_fail_logged >= 1000000) {
            // Раз в секунду, не чаще: отказ приходит покадрово, и полный лог сам
            // стал бы причиной следующего отказа.
            s_flip_fail_logged = finished;
            ESP_LOGW(kTag, "обмен копиями кадра не дождался DMA: %s", esp_err_to_name(flip_err));
        }
        if (flip_err != ESP_OK) {
            ++s_flip_failures;
            shown = false;
            result = flip_err;
        }
    }

    // Кадр дошёл до панели: он прошёл все проверки, в него писали, и копии
    // буфера уже обменялись.
    if (shown) {
        ++s_frames_shown;
        s_shown_rate.tick(esp_timer_get_time());
    }

    xSemaphoreGive(s_lock);

    publish_duty(accum);
    return result;
}

void display_update_brightness(uint8_t brightness) {
    if (s_lock == nullptr) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.brightness = brightness;
    if (s_started) hub75_backend::set_brightness(brightness);
    xSemaphoreGive(s_lock);
}

DisplayStats display_get_stats() {
    DisplayStats stats{};
    stats.free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // Минимум за всё время работы: по текущему значению не видно, насколько
    // близко к дну прибор подходил на тяжёлой конфигурации.
    stats.min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    if (s_lock == nullptr) {
        return stats;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stats.refresh_hz = tach_sample();
    stats.dma_bytes = s_info.dma_bytes;
    stats.bit_depth = s_info.bit_depth;
    stats.effective_bit_depth = s_info.effective_bit_depth;
    stats.commit_us_avg = s_commit.avg_us();
    stats.commit_us_max = s_commit.max_us();
    stats.commit_over_budget = s_commit.over_budget();
    stats.flip_failures = s_flip_failures;
    // Окно частоты вывода двигается и на чтении: без этого частота застыла бы
    // на последнем значении, когда кадры показывать перестали.
    s_shown_rate.roll(esp_timer_get_time());
    stats.frames_shown = s_frames_shown;
    stats.fps_shown = s_shown_rate.fps();
    xSemaphoreGive(s_lock);

    stats.duty = static_cast<float>(s_duty_q16.load(std::memory_order_relaxed)) / 65536.0f;
    return stats;
}

DisplayScanout display_scanout() {
    if (s_lock == nullptr) return {};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ready = s_ready.load(std::memory_order_acquire);
    const DisplayScanout scanout{
        ready ? s_info.scan_period_us : 0u,
        ready ? s_info.passes_per_frame : uint8_t{1},
        s_cfg.buffers > 1,
    };
    xSemaphoreGive(s_lock);
    return scanout;
}

bool display_is_ready() { return s_ready.load(std::memory_order_acquire); }

}  // namespace ledcast
