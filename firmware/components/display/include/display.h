#pragma once

#include <cstdint>
#include <esp_err.h>

#include "deviceconfig.h"

// Вывод на HUB75 за тонким интерфейсом.
//
// Под интерфейсом — ESP32-HUB75-MatrixPanel-DMA: глубина цвета у неё задаётся
// на старте, а не при компиляции, поэтому разрядность меняется без пересборки
// прошивки. Какая именно библиотека внизу, прикладной код не видит.
//
// Координаты — координаты цепочки: x вдоль всей ленты модулей, y внутри модуля.
// Порядок цветов, скан и повороты драйвер получает из конфигурации.

namespace ledcast {

// Exclusive ownership across mapper + driver changes; never nest frame writes.
bool display_config_begin();
void display_config_end();
void display_update_brightness(uint8_t brightness);

// Закрытие кадра ждёт границу прохода развёртки, но дольше кадра на самой
// низкой частоте не длится никогда: такое закрытие — сбой, его считает
// статистика.
inline constexpr uint32_t kCommitBudgetUs = 1000000u / kFrameRates[0];

struct DisplayStats {
    uint32_t refresh_hz;       // реально измеренная частота обновления
    uint32_t dma_bytes;        // занято под битпланы всех копий кадра
    uint32_t free_internal;    // свободной внутренней памяти
    // Минимум свободной внутренней памяти за всё время работы. Текущее значение
    // ничего не говорит о том, насколько близко прибор подходил к дну.
    uint32_t min_free_internal;
    // Кадры, реально доехавшие до панели: счёт идёт после всех проверок и после
    // обмена копиями буфера.
    uint32_t frames_shown;
    uint32_t fps_shown;        // они же за секунду
    // Закрытие кадра вместе с ожиданием границы прохода: среднее и максимум за
    // окно замера. При одной копии оба нуля — ждать нечего.
    uint32_t commit_us_avg;
    uint32_t commit_us_max;
    // Сколько раз закрытие длилось дольше kCommitBudgetUs.
    uint32_t commit_over_budget;
    // Обмен копий DMA не подтвердился до истечения аппаратного дедлайна.
    // Такой кадр не считается показанным.
    uint32_t flip_failures;
    float duty;                // средняя заполненность кадра, 0..1 — для оценки тока
    // Глубина, с которой драйвер поднялся на самом деле. Отличается от той, что
    // лежит в настройках: смену глубины применяет только пересборка драйвера, а
    // она отказывает, пока библиотека держит занятый канал DMA. Показывать
    // настройку вместо факта значит врать о том, чем светит панель.
    uint8_t bit_depth;
    uint8_t effective_bit_depth;
};

esp_err_t display_init(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg);

// Пересобрать драйвер под новые настройки: смена порядка цветов, глубины,
// частоты шины или числа модулей. Кадр при этом гаснет на доли секунды.
esp_err_t display_reconfigure(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg);

// Прогон точек одной строки: count точек подряд от (x, y), шаг по x равен step,
// цвета лежат подряд по три байта RGB.
//
// Всё, что зависит от строки, драйвер считает один раз на прогон, а не на точку,
// поэтому вывод кадра обходится в разы дешевле попиксельного. step равен −1 для
// панели, привинченной вверх ногами: она читает тот же кусок источника справа
// налево.
//
// Берёт замок драйвера одним захватом на прогон: биты RGB лежат в одном слове
// с битами гашения, которые правит смена яркости.
void display_write_run(uint16_t x, uint16_t y, uint16_t count, int8_t step, const uint8_t* rgb);

// Погасить прогон точек — тот же адресный путь, что у display_write_run, но без
// цвета. Отдельной очистки кадра нет: фон под каждую строку кладётся в том же
// проходе, что и картинка, и на него уходит половина работы рендера — считать
// для него чёрный цвет по общему пути незачем.
void display_clear_run(uint16_t x, uint16_t y, uint16_t count, int8_t step);

// Захват полотна внешним источником (видеопоток по сети).
//
// Полотно одно, поэтому два писателя означали бы не наложение, а гонку: пока
// внешний источник держит полотно, движок сцен свой кадр не рисует. Удержание с
// дедлайном, а не флаг — отправитель может пропасть молча, и тогда картинка
// обязана вернуться сама.
void display_external_hold(int64_t until_us);
bool display_external_active();
void display_external_drop();
// Hardware test patterns have priority over every renderer, including NDI.
void display_test_hold(bool active);
bool display_test_active();

// Serialises whole-frame writers. A diagnostic producer sets the test flag before
// waiting for an in-flight NDI frame, so that frame is discarded and can never
// be presented over (or mixed into) the test pattern.
bool display_external_frame_begin();
void display_external_frame_end();
bool display_test_frame_begin();
void display_test_frame_end();

// Кадр нарисован целиком — показать его.
//
// Каждый источник пишет кадр целиком, от первой точки до последней, между
// *_frame_begin и *_frame_end. При двух копиях рисование идёт в ту, которой на
// панели нет; здесь копии меняются местами на ближайшей границе прохода
// развёртки, и возврат совпадает с этой границей. При одной копии обмена нет.
//
// Кадр, в который никто не писал, не показывается. ESP_OK означает, что DMA
// подтвердил переход. При timeout кадр остаётся pending, а следующий
// frame_begin не отдаст CPU ни один буфер до уточнения фактического владельца.
esp_err_t display_commit();

DisplayStats display_get_stats();

// Ритм развёртки. Панель показывает кадр проходами кольца DMA, и обмен копий
// вступает в силу только на границе прохода: кадр, закрытый посреди прохода,
// ждёт его конца. Проход подогнан так, что passes_per_frame проходов — ровно
// период frame_rate, поэтому все источники кадров (видеопоток, автономный
// показ) ставят кадры на одну и ту же сетку через FramePacer.
struct DisplayScanout {
    uint32_t pass_us;          // один проход; 0 — вывод не готов
    uint8_t passes_per_frame;  // проходов на кадр
    bool double_buffered;      // false — кадр пишется прямо в видимую копию
};
DisplayScanout display_scanout();

bool display_is_ready();

}  // namespace ledcast
