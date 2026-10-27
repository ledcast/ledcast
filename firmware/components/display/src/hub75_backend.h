#pragma once

#include <cstdint>

#include <esp_err.h>

#include "deviceconfig.h"

// Внутренний контракт между API display и библиотекой HUB75.
// Всё, что не зависит от библиотеки — статистика, заполненность кадра, замер
// частоты обновления, порядок пересборки — живёт в display.cpp, а вся привязка
// к драйверу собрана в одном файле реализации этих функций; display.h при её
// замене не меняется.
//
// Координаты — координаты цепочки: x вдоль всей ленты модулей, y внутри модуля.
// Отсечение по границам делает вызывающая сторона.

namespace ledcast::hub75_backend {

struct Info {
    uint32_t dma_bytes = 0;    // занято под битпланы всех копий кадра
    uint16_t num_rows = 0;     // адресных строк в кадре
    int8_t addr_msb_gpio = -1; // старшая адресная линия, -1 если замер невозможен
    uint16_t addr_rising_edges_per_refresh = 1; // фронтов PCNT за полный BCM-период
    uint8_t bit_depth = 0;     // глубина, которую драйвер выдаёт на самом деле
    uint8_t effective_bit_depth = 0; // после уплотнения BCM ради refresh rate
    uint32_t scan_period_us = 0; // один проход развёртки; на его границе меняются копии кадра
    uint8_t passes_per_frame = 1; // кадр встаёт на каждый такой проход
};

// Проверка настроек без обращения к железу. Нужна, чтобы решение «новый конфиг
// рабочий» принималось до остановки уже светящейся панели.
esp_err_t validate(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg);

esp_err_t start(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg, Info& info);
void stop();

// Раскладка точек и порядок цветов меняются без остановки драйвера: первое —
// это таблица пересчёта координат, второе — перестановка каналов при записи
// пикселя. Ни то, ни другое не трогает буфер DMA, поэтому подбор раскладки под
// конкретную панель идёт вживую, без перезапуска платы.
esp_err_t set_scan_layout(const ModuleType& module, uint16_t module_count);
void set_color_order(ColorOrder order);

// Прогон точек одной строки; step равен ±1. Всё, что постоянно вдоль строки,
// считается один раз на прогон.
void write_run(uint16_t x, uint16_t y, uint16_t count, int8_t step, const uint8_t* rgb);

// Погасить прогон точек — фон под строку. Отдельно от write_run с чёрным цветом:
// здесь не нужны ни чтение цвета, ни таблица яркости, а фон рисуется каждый кадр
// и стоит половину работы рендера.
void clear_run(uint16_t x, uint16_t y, uint16_t count, int8_t step);

void set_brightness(uint8_t value);

// Обмен копиями кадра. При одной копии не делает ничего.
//
// Нарисованное становится видимым, а на рисование уходит та копия, которую
// панель показывала до этого. Вызывать ровно один раз на кадр, когда кадр
// закрыт, и только под тем же замком, что и запись точек.
//
// Переключение выполняет DMA, а не процессор: flip переставляет ссылку в
// последнем дескрипторе цепочки, и на новую копию вывод переходит, только
// дочитав текущую до конца. Поэтому flip ждёт, пока обмен вступит в силу, —
// извещения от DMA о конце прохода кольца, а не в цикле опроса.
//
// Копирования между копиями нет: каждый источник пишет кадр целиком.
//
// ESP_ERR_TIMEOUT — переход остался pending. Оба буфера закрыты для CPU,
// пока prepare_frame() не подтвердит фактический переход DMA.
esp_err_t flip();

// Resolve a flip that previously timed out before a new frame is allowed to
// write. resolved is true when this call confirmed that pending frame.
esp_err_t prepare_frame(bool& resolved);

}  // namespace ledcast::hub75_backend
