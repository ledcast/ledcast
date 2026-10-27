#pragma once

#include <cstdint>

// Адаптация цвета для панели: перцептивная кривая, упорядоченный дизеринг и
// единственная формула квантования на все пути записи.
//
// Порядок применения обязателен: гамма -> подмешивание порога Bayer ->
// квантование. Гамма ставится до разбиения на битпланы: после квантования она
// растянула бы уже потерянные уровни и дала ступеньки вместо кривой.
//
// Дизеринг упорядоченный, а не диффузией ошибки: диффузия строго
// последовательна вдоль строки, а прогон точек здесь пишется независимо и в
// один буфер. Плитка 4x4 состояния не держит.
//
// Функции лежат в заголовке: межмодульной оптимизации в сборке нет, и вызов
// через границу единицы трансляции не встроился бы в цикл по точкам.

namespace ledcast::colour {

inline constexpr uint8_t kMinDepth = 2;
inline constexpr uint8_t kMaxDepth = 8;

// Обратная sRGB-кривая: 8 бит видеосигнала -> 16 бит линейной яркости.
// В DRAM, а не во flash-rodata: путь горячий, и промах кэша инструкций на
// каждой точке стоил бы дороже самой таблицы.
extern const uint16_t kGamma16[256];

// Смещения порога упорядоченного дизеринга, плитка Bayer 4x4, развёрнутая в
// строку по индексу (y & 3) * 4 + (x & 3). Хранится значение 2 * m + 1, где m —
// ранг ячейки 0..15: так среднее смещение по плитке равно ровно половине шага
// квантования и совпадает с обычным округлением.
extern const uint8_t kBayerBias[16];

// Разрядность разбирается один раз до цикла по точкам: внутри цикла остаются
// только сдвиги и маска, ни одной ветки по глубине.
struct Quantizer {
    uint8_t shift;       // 16 - depth: сдвиг из линейной шкалы в шкалу панели
    uint8_t bias_shift;  // shift - 5: перевод 2 * m + 1 в доли шага квантования
    uint16_t max;        // (1 << depth) - 1: потолок шкалы панели
};

// depth ограничена kMinDepth..kMaxDepth вызывающей стороной, поэтому shift
// лежит в 8..14 и bias_shift не уходит в минус.
inline constexpr Quantizer quantizer_for(uint8_t depth) {
    const uint8_t shift = static_cast<uint8_t>(16u - depth);
    return Quantizer{shift, static_cast<uint8_t>(shift - 5u),
                     static_cast<uint16_t>((1u << depth) - 1u)};
}

// Индекс плитки — только маски, никакого остатка от деления.
inline constexpr uint8_t bayer_index(uint16_t x, uint16_t y) {
    return static_cast<uint8_t>(((y & 3u) << 2) | (x & 3u));
}

// Смещение порога в шкале линейной яркости. Всегда меньше одного шага
// квантования, поэтому плоская заливка размывается не более чем на два соседних
// уровня.
inline constexpr uint16_t bayer_bias(const Quantizer& q, uint8_t bias) {
    return static_cast<uint16_t>(static_cast<uint32_t>(bias) << q.bias_shift);
}

// Смещение обычного округления к ближайшему — для путей без координат точки,
// где дизерингу не за что зацепиться.
inline constexpr uint16_t round_bias(const Quantizer& q) {
    return static_cast<uint16_t>(1u << (q.shift - 1u));
}

// При восьми физических битпланах пространственный дизеринг не добавляет
// полезной разрядности, зато превращает почти чёрные пиксели в заметную сетку
// отдельных светодиодов. На полной глубине округляем равномерно; Bayer нужен
// только когда память или частота вынудили реально сократить число битпланов.
inline constexpr uint16_t quantization_bias(const Quantizer& q, uint8_t depth,
                                            uint16_t x, uint16_t y) {
    return depth >= kMaxDepth ? round_bias(q)
                              : bayer_bias(q, kBayerBias[bayer_index(x, y)]);
}

// Единственная формула квантования: линейная яркость 16 бит плюс смещение
// порога, сдвиг в шкалу панели, потолок. Потолок нужен: сумма переполняет шкалу
// на верхних входах.
inline constexpr uint16_t quantize(uint16_t linear, uint16_t bias, const Quantizer& q) {
    const uint32_t v = (static_cast<uint32_t>(linear) + bias) >> q.shift;
    return v > q.max ? q.max : static_cast<uint16_t>(v);
}

// Весь конвейер на одну составляющую цвета.
inline uint16_t map8(uint8_t value, uint16_t bias, const Quantizer& q) {
    return quantize(kGamma16[value], bias, q);
}

// Значение шкалы панели, разложенное по битпланам: бит d ложится в бит 3d, между
// ними — места под два других канала.
//
// Панель хранит кадр битпланами: на каждом плане у точки лежит тройка RGB. Разбор
// значения бит за битом в цикле по планам — три сдвига, три маски и две сборки на
// каждый план, и это самая дорогая строка всего вывода. Разложение снимается
// таблицей один раз на цвет, а план вынимается из готового слова одним сдвигом.
//
// Таблица посчитана constexpr, а не при старте: инициализация на этапе загрузки
// модуля — скрытый побочный эффект, а значения от неё не зависят.
struct PlaneSpread {
    uint32_t value[256];
};

inline constexpr PlaneSpread make_plane_spread() {
    PlaneSpread table{};
    for (uint32_t v = 0; v < 256u; ++v) {
        uint32_t spread = 0;
        for (uint32_t d = 0; d < 8u; ++d) {
            spread |= ((v >> d) & 1u) << (3u * d);
        }
        table.value[v] = spread;
    }
    return table;
}

extern const PlaneSpread kPlaneSpread;

inline uint32_t plane_spread(uint16_t value) {
    return kPlaneSpread.value[value & 0xFFu];
}

// Слово прогона: тройка каналов на плане d вынимается как (word >> 3 * d) & 7,
// порядок бит внутри тройки — красный, зелёный, синий, как его ждёт панель.
inline uint32_t plane_word(uint16_t r, uint16_t g, uint16_t b) {
    return plane_spread(r) | (plane_spread(g) << 1) | (plane_spread(b) << 2);
}

}  // namespace ledcast::colour
