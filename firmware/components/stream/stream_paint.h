#pragma once

#include <cstdint>

#include "deviceconfig.h"

// Вывод разобранного отрезка видеопотока на линейное полотно LEDCAST.

namespace ledcast::stream {

struct Canvas {
    uint16_t width;
    uint16_t height;
};

// Полотно, которого узел ждёт от отправителя, для выбранного режима адресации.
Canvas paint_canvas();

// Отрезок строки: count точек подряд от (x, y), цвета лежат по
// три байта RGB.
void paint_span(uint16_t x, uint16_t y, uint16_t count, const uint8_t* rgb);

}  // namespace ledcast::stream
