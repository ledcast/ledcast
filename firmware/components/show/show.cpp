#include "show.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "display.h"
#include "frame_pacer.h"
#include "linear.h"
#include "show_blend.h"
#include "show_font.h"
#include "show_timing.h"

namespace ledcast {
namespace {

constexpr char kTag[] = "show";
constexpr uint8_t kGlyphRows = 7;
constexpr uint8_t kGlyphColumns = 5;
constexpr int kShowPriority = 20;
// Зазор между фразами на ленте — три клетки знака. Пробел между словами
// занимает одну клетку, поэтому граница фраз читается заметно шире словесной,
// но панель не пустеет. Клетка равна кеглю, так что зазор растёт с ним.
constexpr uint8_t kPhraseGapCells = 3;
// Вывод пересобирается или ещё не поднят — следующая попытка через это время.
constexpr int64_t kRetryUs = 10000;

SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_task = nullptr;
esp_timer_handle_t s_wake_timer = nullptr;
ShowConfig s_config{};
std::atomic<bool> s_enabled{false};
std::atomic<bool> s_paused{false};
std::atomic<uint32_t> s_generation{0};

using show_font::glyph;
using show_font::utf8_next;

constexpr size_t kMaxCachedGlyphs = kShowTextBytes;
constexpr size_t kMaxCachedPhrases = (kShowTextBytes + 1) / 2 + 1;

struct CachedPhrase {
    uint16_t glyph_begin = 0;
    uint16_t glyph_count = 0;
    int32_t width_px = 0;
    uint32_t ribbon_start_px = 0;  // начало блока фразы на круге бегущей строки
    uint32_t cell_px = 0;          // одна копия фразы вместе с зазором за ней
};

// Столбец текста — маска строк экрана, бит y светится. Высота модуля не больше
// 64 точек, поэтому столбец целиком помещается в одно слово, и кадр собирается
// по столбцам, а не по точкам: глиф разбирается один раз на столбец.
using Column = uint64_t;
static_assert(kMaxModuleHeight <= 64, "столбец текста — одно 64-битное слово");

struct TextCache {
    const uint8_t* glyphs[kMaxCachedGlyphs]{};
    CachedPhrase phrases[kMaxCachedPhrases]{};
    uint16_t glyph_count = 0;
    uint16_t phrase_count = 0;
    uint16_t panel_width = 0;
    uint16_t panel_height = 0;
    uint8_t glyph_width_px = 5;
    uint8_t glyph_advance_px = 7;
    uint32_t ribbon_period_px = 0;  // длина круга ленты: фразы и зазоры
    // Масштаб шрифта снят в таблицы: столбец шрифта под каждым столбцом клетки
    // и строки экрана, которые рисует каждая строка шрифта. Выборка — по
    // центрам светодиодов, как в show_font::pixel.
    uint8_t font_column[kMaxModuleHeight]{};
    Column row_mask[kGlyphRows]{};
};

TextCache s_cache{};

void build_cache(const ShowConfig& cfg, TextCache& cache) {
    cache = {};
    cache.panel_width = linear_width();
    cache.panel_height = linear_height();
    const uint8_t font_height = std::max<uint8_t>(
        kGlyphRows, static_cast<uint8_t>(std::min<uint16_t>(cfg.font_size_px, cache.panel_height)));
    cache.glyph_width_px =
        static_cast<uint8_t>((kGlyphColumns * font_height + kGlyphRows - 1) / kGlyphRows);
    cache.glyph_advance_px = font_height;
    for (uint8_t x = 0; x < cache.glyph_width_px; ++x)
        cache.font_column[x] =
            static_cast<uint8_t>((2u * x + 1u) * kGlyphColumns / (2u * cache.glyph_width_px));
    const int origin_y = (static_cast<int>(cache.panel_height) - font_height) / 2;
    for (uint8_t y = 0; y < font_height; ++y) {
        const int screen_y = origin_y + y;
        if (screen_y < 0 || screen_y >= cache.panel_height) continue;
        cache.row_mask[(2u * y + 1u) * kGlyphRows / (2u * font_height)] |= Column{1} << screen_y;
    }

    const uint32_t gap_px = static_cast<uint32_t>(kPhraseGapCells) * cache.glyph_advance_px;
    uint32_t ribbon_end = 0;
    const char* line = cfg.text;
    for (const char* cursor = cfg.text;; ++cursor) {
        if (*cursor != '\n' && *cursor != '\0') continue;
        const char* end = cursor;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) --end;
        const char* begin = line;
        while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
        if (begin < end && cache.phrase_count < kMaxCachedPhrases) {
            CachedPhrase& phrase = cache.phrases[cache.phrase_count];
            phrase.glyph_begin = cache.glyph_count;
            const char* p = begin;
            while (p < end && cache.glyph_count < kMaxCachedGlyphs) {
                cache.glyphs[cache.glyph_count++] = glyph(utf8_next(p, end));
                ++phrase.glyph_count;
            }
            phrase.width_px = static_cast<int32_t>(phrase.glyph_count) * cache.glyph_advance_px;
            phrase.ribbon_start_px = ribbon_end;
            phrase.cell_px = static_cast<uint32_t>(phrase.width_px) + gap_px;
            ribbon_end += phrase.cell_px * show_timing::phrase_repeats(phrase.cell_px, cfg.speed_px_s,
                                                                       cfg.duration_s);
            ++cache.phrase_count;
        }
        if (*cursor == '\0') break;
        line = cursor + 1;
    }
    cache.ribbon_period_px = ribbon_end;
}

Column phrase_column(const TextCache& cache, const CachedPhrase& phrase, int32_t x) {
    if (x < 0 || x >= phrase.width_px) return 0;
    const uint16_t glyph_index = static_cast<uint16_t>(x / cache.glyph_advance_px);
    const uint8_t local_x = static_cast<uint8_t>(x % cache.glyph_advance_px);
    if (local_x >= cache.glyph_width_px) return 0;
    const uint8_t* rows = cache.glyphs[phrase.glyph_begin + glyph_index];
    const uint8_t bit = static_cast<uint8_t>(1u << (kGlyphColumns - 1u - cache.font_column[local_x]));
    Column column = 0;
    for (uint8_t row = 0; row < kGlyphRows; ++row)
        if (rows[row] & bit) column |= cache.row_mask[row];
    return column;
}

// Что стоит в кадре: целая часть сдвига задаёт столбцы, дробная — долю, с
// которой точка берёт свет у соседнего справа столбца.
struct Placement {
    bool scrolling = false;
    uint64_t travelled_px = 0;           // бегущая строка: целая часть сдвига ленты
    uint8_t fraction = 0;                // бегущая строка: доля следующего столбца, 1/256
    const CachedPhrase* still = nullptr; // статика: фраза по центру
    int32_t still_origin = 0;
};

Column frame_column(const TextCache& cache, const Placement& at, int x) {
    if (!at.scrolling) return at.still != nullptr ? phrase_column(cache, *at.still, x - at.still_origin) : 0;
    if (cache.phrase_count == 0) return 0;
    const int64_t u = show_timing::ribbon_x(at.travelled_px, cache.panel_width, cache.ribbon_period_px, x);
    if (u < 0) return 0;
    const uint16_t index = show_timing::ribbon_phrase(cache.phrase_count, static_cast<uint32_t>(u),
        [&](uint16_t n) { return cache.phrases[n].ribbon_start_px; });
    const CachedPhrase& phrase = cache.phrases[index];
    return phrase_column(cache, phrase, static_cast<int32_t>((u - phrase.ribbon_start_px) % phrase.cell_px));
}

bool running() {
    return s_enabled.load(std::memory_order_acquire) && !s_paused.load(std::memory_order_acquire);
}

// Спать до deadline_us. Будят и таймер, и смена настроек; лишнее пробуждение
// не укорачивает сон — по нему кадр закрылся бы на проход раньше своего.
// Смена настроек или пауза прерывают сон сразу.
void sleep_until(int64_t deadline_us, uint32_t generation) {
    for (;;) {
        const int64_t left = deadline_us - esp_timer_get_time();
        if (left <= 0 || !running() || s_generation.load(std::memory_order_acquire) != generation) return;
        esp_timer_stop(s_wake_timer);
        esp_timer_start_once(s_wake_timer, static_cast<uint64_t>(left));
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

// Нарисовать кадр целиком и закрыть его не раньше commit_at_us. ESP_OK — кадр
// на панели: при двух копиях возврат совпадает с границей прохода развёртки.
esp_err_t draw(const ShowConfig& cfg, const TextCache& cache, const Placement& at,
               int64_t commit_at_us, uint32_t generation) {
    const uint16_t width = cache.panel_width, height = cache.panel_height;
    if (width == 0 || height == 0) return ESP_ERR_INVALID_STATE;
    if (!display_test_frame_begin()) return ESP_ERR_INVALID_STATE;
    // Тест или пересборка могли случиться, пока задача ждала свободную копию
    // кадра: не затирать выбранный тест и не рисовать по старой геометрии.
    if (!running() || linear_width() != width || linear_height() != height) {
        display_test_frame_end();
        return ESP_ERR_INVALID_STATE;
    }
    // Четыре цвета на кадр: точка гаснет, светит только свой столбец, только
    // соседний или оба. Доля у всех точек кадра одна и та же.
    const uint32_t right = at.fraction, left = show_timing::kSubpixels - right;
    const uint8_t palette[4][3] = {
        {0, 0, 0},
        {show_blend::dimmed(cfg.red, left), show_blend::dimmed(cfg.green, left), show_blend::dimmed(cfg.blue, left)},
        {show_blend::dimmed(cfg.red, right), show_blend::dimmed(cfg.green, right), show_blend::dimmed(cfg.blue, right)},
        {cfg.red, cfg.green, cfg.blue},
    };
    constexpr uint16_t kChunk = 64;
    Column columns[kChunk + 1];
    uint8_t rgb[kChunk * 3];
    for (uint16_t x = 0; x < width; x += kChunk) {
        const uint16_t amount = std::min<uint16_t>(kChunk, width - x);
        for (uint16_t i = 0; i <= amount; ++i) columns[i] = frame_column(cache, at, x + i);
        for (uint16_t y = 0; y < height; ++y) {
            const Column bit = Column{1} << y;
            for (uint16_t i = 0; i < amount; ++i) {
                const uint8_t shade = ((columns[i] & bit) != 0 ? 1u : 0u) | ((columns[i + 1] & bit) != 0 ? 2u : 0u);
                std::memcpy(rgb + i * 3, palette[shade], 3);
            }
            linear_paint(x, y, amount, rgb);
        }
    }
    sleep_until(commit_at_us, generation);
    const esp_err_t err = display_commit();
    display_test_frame_end();
    return err;
}

// Статика: фраза по центру, смена каждые duration_s. Кадр рисуется один раз
// на фразу, дальше задача спит до смены.
void still_frame(const ShowConfig& cfg, const TextCache& cache, int64_t epoch_us, uint32_t generation) {
    const int64_t now = esp_timer_get_time();
    if (cache.phrase_count == 0) {
        // Текста нет — панель гаснет и ждёт новых настроек.
        if (draw(cfg, cache, Placement{}, now, generation) != ESP_OK) {
            sleep_until(now + kRetryUs, generation);
            return;
        }
        while (running() && s_generation.load(std::memory_order_acquire) == generation)
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        return;
    }
    const uint64_t duration_us = static_cast<uint64_t>(cfg.duration_s) * 1000000ULL;
    const uint64_t elapsed = static_cast<uint64_t>(now - epoch_us);
    Placement at{};
    at.still = &cache.phrases[(elapsed / duration_us) % cache.phrase_count];
    at.still_origin = (static_cast<int32_t>(cache.panel_width) - at.still->width_px) / 2;
    if (draw(cfg, cache, at, now, generation) != ESP_OK) {
        sleep_until(now + kRetryUs, generation);
        return;
    }
    sleep_until(epoch_us + static_cast<int64_t>((elapsed / duration_us + 1) * duration_us), generation);
}

// Бегущая строка. Кадр встаёт на общую сетку кадров вывода, а картинка
// считается на тот момент, когда кадр станет виден, а не когда его начали
// рисовать.
void scroll_frame(const ShowConfig& cfg, const TextCache& cache, int64_t epoch_us, uint32_t generation,
                  FramePacer& pacer) {
    const DisplayScanout scanout = display_scanout();
    const int64_t now = esp_timer_get_time();
    if (scanout.pass_us == 0) {
        pacer.lost();
        sleep_until(now + kRetryUs, generation);
        return;
    }
    const int64_t present = pacer.next_present(scanout.pass_us, scanout.passes_per_frame, now);
    const uint64_t travelled = show_timing::travelled_q8(
        static_cast<uint64_t>(std::max<int64_t>(0, present - epoch_us)), cfg.speed_px_s);
    Placement at{};
    at.scrolling = true;
    at.travelled_px = travelled / show_timing::kSubpixels;
    at.fraction = static_cast<uint8_t>(travelled % show_timing::kSubpixels);

    // При одной копии кадр виден уже во время рисования, и закрытие только
    // отмеряет ритм.
    const int64_t commit_at = scanout.double_buffered
        ? FramePacer::commit_after(scanout.pass_us, present) : present;
    const esp_err_t err = draw(cfg, cache, at, commit_at, generation);
    if (err != ESP_OK) {
        pacer.lost();
        if (err == ESP_ERR_INVALID_STATE) sleep_until(esp_timer_get_time() + kRetryUs, generation);
        return;
    }
    pacer.presented(scanout.double_buffered ? esp_timer_get_time() : present);
}

void task(void*) {
    uint32_t generation = UINT32_MAX;
    int64_t epoch_us = 0;
    FramePacer pacer;
    ShowConfig snapshot{};
    for (;;) {
        const uint32_t current = s_generation.load(std::memory_order_acquire);
        if (current != generation) {
            generation = current;
            xSemaphoreTake(s_lock, portMAX_DELAY); snapshot = s_config; xSemaphoreGive(s_lock);
            build_cache(snapshot, s_cache);
            epoch_us = esp_timer_get_time();
            pacer.lost();
        }
        if (!running()) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (snapshot.mode == ShowMode::ScrollRightToLeft) {
            scroll_frame(snapshot, s_cache, epoch_us, generation, pacer);
        } else {
            still_frame(snapshot, s_cache, epoch_us, generation);
        }
    }
}

void wake() { if (s_task != nullptr) xTaskNotifyGive(s_task); }

}  // namespace

esp_err_t show_start(const ShowConfig& config) {
    if (s_lock == nullptr) s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_NO_MEM, kTag, "show mutex");
    xSemaphoreTake(s_lock, portMAX_DELAY); s_config = config; xSemaphoreGive(s_lock);
    s_enabled.store(config.enabled, std::memory_order_release);
    display_test_hold(config.enabled);
    if (s_wake_timer == nullptr) {
        const esp_timer_create_args_t timer_args{
            .callback = [](void*) { wake(); },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "show-wake",
            .skip_unhandled_events = true,
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_wake_timer), kTag, "show timer");
    }
    if (s_task == nullptr) {
        ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(task, "show", 8192, nullptr, kShowPriority, &s_task, 1) == pdPASS,
                            ESP_ERR_NO_MEM, kTag, "show task");
    }
    ++s_generation; wake();
    ESP_LOGI(kTag, "autonomous show %s", config.enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t show_reconfigure(const ShowConfig& config) {
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_INVALID_STATE, kTag, "show not started");
    const bool was_enabled = s_enabled.load(std::memory_order_acquire);
    xSemaphoreTake(s_lock, portMAX_DELAY); s_config = config; xSemaphoreGive(s_lock);
    s_enabled.store(config.enabled, std::memory_order_release);
    if (config.enabled) {
        display_test_hold(true);
    } else if (was_enabled && !s_paused.load(std::memory_order_acquire)) {
        if (display_test_frame_begin()) { linear_blackout(); display_test_frame_end(); }
        display_test_hold(false); display_external_drop();
    }
    ++s_generation; wake();
    return ESP_OK;
}

bool show_active() { return s_enabled.load(std::memory_order_acquire); }

void show_pause_for_test(bool paused) {
    s_paused.store(paused, std::memory_order_release);
    if (!paused) ++s_generation;
    wake();
}

void show_redraw() { ++s_generation; wake(); }

}  // namespace ledcast
