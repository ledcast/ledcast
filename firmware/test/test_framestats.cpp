#include "check.h"
#include "framestats.h"

using namespace ledcast;

namespace {

void test_timing_window_publishes_only_on_close() {
    TimingWindow w(1000000, 13000);
    w.reset(0);

    w.add(100, 100000);
    w.add(300, 200000);
    // Окно ещё не закрыто: наружу уходит прошлое значение, а не половина замера.
    CHECK_EQ(w.avg_us(), 0);
    CHECK_EQ(w.max_us(), 0);

    w.add(500, 1000000);
    CHECK_EQ(w.avg_us(), 300);  // (100 + 300 + 500) / 3
    CHECK_EQ(w.max_us(), 500);
}

void test_timing_window_counts_over_budget() {
    TimingWindow w(1000000, 13000);
    w.reset(0);

    w.add(13000, 1000);   // ровно бюджет — ещё не превышение
    w.add(13001, 2000);
    w.add(20000, 3000);
    CHECK_EQ(w.over_budget(), 2);

    // Счёт накопительный: закрытие окна его не обнуляет, иначе редкие срывы
    // кадра исчезали бы между двумя чтениями статуса.
    w.add(1000, 1000000);
    CHECK_EQ(w.over_budget(), 2);
    w.add(99000, 1100000);
    CHECK_EQ(w.over_budget(), 3);
}

void test_timing_window_idle_gives_honest_zero() {
    TimingWindow w(1000000, 13000);
    w.reset(0);
    w.add(400, 1000);
    w.add(600, 1000000);
    CHECK_EQ(w.avg_us(), 500);

    // Кадров не было вовсе: в окне без замеров среднее и максимум обязаны стать
    // нулём, а не держать прошлое значение.
    w.idle(1500000);
    CHECK_EQ(w.avg_us(), 500);
    w.idle(2000000);
    CHECK_EQ(w.avg_us(), 0);
    CHECK_EQ(w.max_us(), 0);
}

void test_rate_window_counts_by_elapsed_not_by_ticks() {
    RateWindow r(1000000);
    r.reset(0);

    // Кадр каждые 33 мс — это 30.3 к/с. Окно закрывается на 31-м кадре, когда
    // прошло уже 1.023 с: счётчик кадров показал бы 31, то есть систематически
    // больше правды. Считаем по прошедшему времени.
    for (int k = 1; k <= 31; ++k) {
        r.tick(static_cast<int64_t>(k) * 33000);
    }
    CHECK_EQ(r.fps(), 30);
}

void test_rate_window_drops_to_zero_without_frames() {
    RateWindow r(1000000);
    r.reset(0);
    for (int k = 1; k <= 60; ++k) {
        r.tick(static_cast<int64_t>(k) * 16667);
    }
    CHECK(r.fps() > 0);

    r.roll(3000000);
    CHECK_EQ(r.fps(), 0);
}

void test_sample_ring_keeps_last_32() {
    SampleRing ring;
    CHECK_EQ(ring.avg(), 0);
    CHECK_EQ(ring.max(), 0);
    CHECK_EQ(ring.spread(), 0);

    // Первые три отсчёта обязаны выпасть: кольцо держит ровно 32 последних.
    for (uint32_t v = 1; v <= 35; ++v) {
        ring.push(v * 10);
    }
    CHECK_EQ(ring.max(), 350);
    // Значения 40..350 с шагом 10: среднее ровно 195.
    CHECK_EQ(ring.avg(), 195);
    // Наибольшее отклонение от среднего — на краю: 350 - 195 = 155.
    CHECK_EQ(ring.spread(), 155);
}

void test_sample_ring_spread_is_zero_on_even_stream() {
    SampleRing ring;
    for (int i = 0; i < 32; ++i) {
        ring.push(16667);
    }
    CHECK_EQ(ring.avg(), 16667);
    CHECK_EQ(ring.max(), 16667);
    CHECK_EQ(ring.spread(), 0);
}

}  // namespace

int main() {
    test_timing_window_publishes_only_on_close();
    test_timing_window_counts_over_budget();
    test_timing_window_idle_gives_honest_zero();
    test_rate_window_counts_by_elapsed_not_by_ticks();
    test_rate_window_drops_to_zero_without_frames();
    test_sample_ring_keeps_last_32();
    test_sample_ring_spread_is_zero_on_even_stream();
    return hosttest::report("framestats");
}
