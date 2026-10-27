#pragma once

#include <cstdint>

#include "deviceconfig.h"

// Распиновка порта X1 платы Huidu HD-WF2 и раскладка цветовых линий.
// Файл не знает ничего о конкретной библиотеке HUB75: это факты о железе.

namespace ledcast::board {

inline constexpr int8_t kPinR1 = 2;
inline constexpr int8_t kPinG1 = 6;
inline constexpr int8_t kPinB1 = 10;
inline constexpr int8_t kPinR2 = 3;
inline constexpr int8_t kPinG2 = 7;
inline constexpr int8_t kPinB2 = 11;
inline constexpr int8_t kPinA = 39;
inline constexpr int8_t kPinB = 38;
inline constexpr int8_t kPinC = 37;
inline constexpr int8_t kPinD = 36;
inline constexpr int8_t kPinE = 21;
inline constexpr int8_t kPinLat = 33;
inline constexpr int8_t kPinOe = 35;
inline constexpr int8_t kPinClk = 34;

// Адресные линии по возрастанию веса. Старшая из используемых поднимается
// ровно один раз за кадр — на этом построен замер частоты обновления.
inline constexpr int8_t kAddrPins[5] = {kPinA, kPinB, kPinC, kPinD, kPinE};

struct DataPins {
    int8_t r1, g1, b1;
    int8_t r2, g2, b2;
};

// Порядок цветов — перестановка проводов, а не преобразование данных.
// Буквы ColorOrder перечисляют, какой цвет висит на первой, второй и третьей
// линии модуля; драйверу нужен обратный ответ — какая линия несёт красный.
// Отсюда таблица позиций R, G и B внутри названия перестановки.
constexpr DataPins data_pins_for(ColorOrder order) {
    constexpr uint8_t kLaneOfComponent[6][3] = {
        {0, 1, 2},  // RGB
        {0, 2, 1},  // RBG
        {1, 0, 2},  // GRB
        {2, 0, 1},  // GBR
        {1, 2, 0},  // BRG
        {2, 1, 0},  // BGR
    };
    constexpr int8_t kUpper[3] = {kPinR1, kPinG1, kPinB1};
    constexpr int8_t kLower[3] = {kPinR2, kPinG2, kPinB2};

    const uint8_t* lane = kLaneOfComponent[static_cast<uint8_t>(order)];
    return DataPins{kUpper[lane[0]], kUpper[lane[1]], kUpper[lane[2]],
                    kLower[lane[0]], kLower[lane[1]], kLower[lane[2]]};
}

static_assert(data_pins_for(ColorOrder::BGR).r1 == kPinB1, "BGR подаёт красный на линию B");
static_assert(data_pins_for(ColorOrder::BGR).b1 == kPinR1, "BGR подаёт синий на линию R");
static_assert(data_pins_for(ColorOrder::RGB).r1 == kPinR1, "RGB не переставляет линии");

}  // namespace ledcast::board
