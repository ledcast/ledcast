#include "check.h"
#include "show_timing.h"

#include <cstdint>
#include <vector>

namespace {
using namespace ledcast::show_timing;

// Модель ленты, как её собирает show.cpp: фразы подряд, после каждой зазор.
struct Ribbon {
    std::vector<uint32_t> start, width;
    uint32_t period = 0;
    Ribbon(std::vector<uint32_t> widths, uint32_t gap) : width(widths) {
        for (uint32_t w : widths) { start.push_back(period); period += w + gap; }
    }
    // Что видно в столбце x: -1 — пусто (ещё не въехала или зазор),
    // иначе фраза * 10000 + столбец внутри неё.
    int64_t at(uint64_t travelled, uint16_t panel, int x) const {
        const int64_t u = ribbon_x(travelled, panel, period, x);
        if (u < 0) return -1;
        const uint16_t i = ribbon_phrase(static_cast<uint16_t>(start.size()), static_cast<uint32_t>(u),
            [&](uint16_t n) { return start[n]; });
        const int64_t local = u - start[i];
        return local < width[i] ? i * 10000 + local : -1;
    }
};

// Лента из блоков, как её собирает show.cpp при duration_s > 0: в блоке —
// копии одной фразы, после каждой копии зазор.
struct Blocks {
    std::vector<uint32_t> start, width, cell;
    uint32_t period = 0;
    Blocks(std::vector<uint32_t> widths, uint32_t gap, uint16_t speed, uint16_t duration) : width(widths) {
        for (uint32_t w : widths) {
            start.push_back(period);
            cell.push_back(w + gap);
            period += (w + gap) * phrase_repeats(w + gap, speed, duration);
        }
    }
    int64_t at(uint64_t travelled, uint16_t panel, int x) const {
        const int64_t u = ribbon_x(travelled, panel, period, x);
        if (u < 0) return -1;
        const uint16_t i = ribbon_phrase(static_cast<uint16_t>(start.size()), static_cast<uint32_t>(u),
            [&](uint16_t n) { return start[n]; });
        const int64_t local = (u - start[i]) % cell[i];
        return local < width[i] ? i * 10000 + local : -1;
    }
};

// Каждый шаг на пиксель — ровно сдвиг картинки на столбец влево, на всём
// протяжении нескольких кругов, включая смены фраз и стык последней с первой.
void check_continuous(const Ribbon& r, uint16_t panel) {
    for (uint64_t p = 0; p < 4ull * r.period + panel; ++p)
        for (int x = 0; x + 1 < panel; ++x)
            CHECK_EQ(r.at(p + 1, panel, x), r.at(p, panel, x + 1));
}
}  // namespace

int main() {
    // Позиция в долях точки: ровно speed точек за секунду и ни одного шага
    // назад, на всех допустимых скоростях.
    for (unsigned speed = 1; speed <= 240; ++speed) {
        CHECK_EQ(travelled_q8(1000000u, speed), speed * kSubpixels);
        CHECK_EQ(travelled_q8(3600000000ull, speed), 3600ull * speed * kSubpixels);
        uint64_t previous = 0;
        for (uint64_t t = 0; t < 200000u; t += 997u) {
            const uint64_t now = travelled_q8(t, static_cast<uint16_t>(speed));
            CHECK(now >= previous);
            previous = now;
        }
    }

    // Кадры на равномерной сетке проходят равные доли пути: на 70 точках/с
    // при 120 кадрах шаг кадра отличается от соседнего не больше чем на 1/256.
    for (unsigned speed : {7u, 50u, 70u, 240u}) {
        const uint64_t step_us = 8333;
        const uint64_t ideal = (step_us * speed * kSubpixels) / 1000000u;
        for (uint64_t n = 1; n < 5000; ++n) {
            const uint64_t advance = travelled_q8(n * step_us, static_cast<uint16_t>(speed)) -
                                     travelled_q8((n - 1) * step_us, static_cast<uint16_t>(speed));
            CHECK(advance == ideal || advance == ideal + 1);
        }
    }

    // Первый въезд: лента входит справа, левее головы пусто.
    const Ribbon two({30, 50}, 21);
    CHECK_EQ(two.at(0, 64, 63), -1);
    CHECK_EQ(two.at(1, 64, 63), 0);
    CHECK_EQ(two.at(1, 64, 62), -1);

    // Фраза → следующая: вторая идёт сразу за первой через зазор, без скачка.
    check_continuous(two, 64);
    CHECK_EQ(two.at(64 + 30 + 21, 64, 0), 10000);       // голова второй фразы
    CHECK_EQ(two.at(64 + 30 + 21 - 1, 64, 0), -1);      // перед ней зазор
    CHECK_EQ(two.at(64 + 29, 64, 0), 29);               // хвост первой

    // Последняя → первая: за хвостом второй тот же зазор и снова первая.
    const uint64_t lap = 64 + two.period;
    CHECK_EQ(two.at(lap - 21 - 1, 64, 0), 10000 + 49);
    CHECK_EQ(two.at(lap - 1, 64, 0), -1);
    CHECK_EQ(two.at(lap, 64, 0), 0);
    for (uint64_t p = 64; p < 64 + 2 * two.period; ++p)
        for (int x = 0; x < 64; ++x)
            CHECK_EQ(two.at(p + two.period, 64, x), two.at(p, 64, x));

    // Одна фраза идёт сама за собой бесконечно: и когда круг уже панели,
    // и когда длиннее, — без перезапуска с правого края.
    for (uint32_t w : {10u, 40u, 200u}) {
        const Ribbon one({w}, 21);
        check_continuous(one, 64);
        for (uint64_t p = 64; p < 64 + 3 * one.period; ++p)
            for (int x = 0; x < 64; ++x)
                CHECK_EQ(one.at(p + one.period, 64, x), one.at(p, 64, x));
        CHECK_EQ(one.at(64 + one.period, 64, 0), 0);
        CHECK_EQ(one.at(64 + one.period - 1, 64, 0), -1);
    }
    const Ribbon three({12, 1, 70}, 9);
    check_continuous(three, 128);

    // Длительность фразы: 0 — одна копия, фразы по очереди; иначе столько
    // копий, сколько лента проезжает за это время, но не меньше одной.
    CHECK_EQ(phrase_repeats(51, 50, 0), 1u);
    CHECK_EQ(phrase_repeats(51, 50, 8), 8u);    // 400 px / 51 = 7.8
    CHECK_EQ(phrase_repeats(71, 50, 8), 6u);    // 400 px / 71 = 5.6
    CHECK_EQ(phrase_repeats(500, 10, 1), 1u);
    CHECK_EQ(phrase_repeats(0, 50, 8), 1u);

    // Блоки: восемь копий первой фразы, потом шесть второй, и снова первая —
    // лента сдвигается ровно на столбец на каждом шаге, стыки блоков без рывка.
    const Blocks blocks({30, 50}, 21, 50, 8);
    CHECK_EQ(blocks.period, 8u * 51u + 6u * 71u);
    for (uint64_t p = 0; p < 3ull * blocks.period + 64; ++p)
        for (int x = 0; x + 1 < 64; ++x)
            CHECK_EQ(blocks.at(p + 1, 64, x), blocks.at(p, 64, x + 1));
    // Вторая копия первой фразы стоит сразу за первой через зазор.
    CHECK_EQ(blocks.at(64 + 51, 64, 0), 0);
    // После восьмой копии первой — голова второй фразы.
    CHECK_EQ(blocks.at(64 + 8 * 51, 64, 0), 10000);
    CHECK_EQ(blocks.at(64 + 8 * 51 - 1, 64, 0), -1);

    return hosttest::report("show_timing");
}
