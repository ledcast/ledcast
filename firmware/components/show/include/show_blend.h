#pragma once
#include <cstdint>

#include "colour.h"

namespace ledcast::show_blend {
// Код sRGB, который панель покажет с яркостью share/256 от яркости кода value.
//
// Точка между двумя столбцами глифа светит долей каждого из них, и доли
// складываются в линейном свете, а не в кодах: панель сама переводит код в ШИМ
// по kGamma16, и половина кода дала бы не половину света, а пятую часть — край
// на полпути между столбцами тускнел бы, и текст мерцал на ходу.
inline uint8_t dimmed(uint8_t value, uint32_t share) {
    if (share >= 256u) return value;
    const uint32_t target = (static_cast<uint32_t>(colour::kGamma16[value]) * share + 128u) >> 8;
    // kGamma16 монотонна: ближайший код ищется делением пополам.
    uint32_t lo = 0, hi = value;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2u;
        if (colour::kGamma16[mid] < target) lo = mid + 1u; else hi = mid;
    }
    if (lo > 0 && target - colour::kGamma16[lo - 1u] < colour::kGamma16[lo] - target) --lo;
    return static_cast<uint8_t>(lo);
}
}
