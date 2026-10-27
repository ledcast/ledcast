#pragma once
#include <cstdint>

namespace ledcast::show_timing {
// Позиция ленты хранится в 1/256 точки. Сдвиг ступеньками в целую точку на
// скорости, не кратной частоте кадров, идёт неровно — то через кадр, то через
// два, — и на 70 точек/с при 61 Гц текст ехал рывками по 1–2 точки.
inline constexpr uint32_t kSubpixels = 256;

// Сколько проехала лента к моменту elapsed_us, в 1/256 точки. Позиция считается
// от абсолютного времени показа, поэтому опоздавший кадр не копит ошибку.
constexpr uint64_t travelled_q8(uint64_t elapsed_us, uint16_t speed) {
    return elapsed_us * speed * kSubpixels / 1000000u;
}

// Сколько копий фразы идёт подряд, прежде чем въедет следующая: столько,
// сколько лента проезжает за duration_s. cell_px — фраза вместе с зазором за
// ней. 0 секунд — одна копия: фразы идут по очереди.
constexpr uint32_t phrase_repeats(uint32_t cell_px, uint16_t speed, uint16_t duration_s) {
    if (duration_s == 0 || cell_px == 0) return 1;
    const uint64_t copies = (uint64_t(speed) * duration_s + cell_px / 2u) / cell_px;
    return copies == 0 ? 1u : static_cast<uint32_t>(copies);
}

// Бегущая строка — одна бесконечная лента: блоки фраз подряд (в блоке — копии
// одной фразы, после каждой зазор), за последним блоком снова первый. period_px — длина одного круга ленты (сумма
// ширин фраз и зазоров). Возвращает координату на круге под столбцом экрана x
// или -1, пока голова ленты при первом въезде ещё не доехала до столбца.
// Время отсчитывается от включения показа и никогда не сбрасывается на смене
// фразы, поэтому стыков между фразами и кругами нет.
constexpr int64_t ribbon_x(uint64_t travelled, uint16_t panel_width, uint32_t period_px, int x) {
    const int64_t u = static_cast<int64_t>(x) + static_cast<int64_t>(travelled) - panel_width;
    if (u < 0 || period_px == 0) return -1;
    return u % period_px;
}

// Фраза, которой принадлежит координата круга u: последняя с началом <= u.
// start_of(i) — начало i-й фразы на круге, растёт с i, start_of(0) == 0.
template <class StartOf>
constexpr uint16_t ribbon_phrase(uint16_t count, uint32_t u, StartOf start_of) {
    uint16_t lo = 0, hi = count;
    while (hi - lo > 1) {
        const uint16_t mid = static_cast<uint16_t>((lo + hi) / 2);
        if (start_of(mid) <= u) lo = mid; else hi = mid;
    }
    return lo;
}
}
