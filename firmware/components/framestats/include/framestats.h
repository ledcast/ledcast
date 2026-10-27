#pragma once

#include <cstdint>

// Замер производительности вывода окном.
//
// Мгновенный сэмпл времени рендера бесполезен: на пустой сцене он гуляет от 16
// до 1255 мкс тремя кластерами — это разные ветви вызова, а не шум, и одно
// число из такой выборки не значит ничего. Наружу поэтому уходят среднее и
// максимум за окно замера плюс накопительный счёт кадров, вышедших за бюджет.
//
// Файл намеренно не знает ни про FreeRTOS, ни про таймер платы: время приходит
// параметром, поэтому вся арифметика собирается на хосте под тесты.

namespace ledcast {

// Среднее и максимум за окно. Пока окно не закрылось, наружу уходит прошлое
// значение: половина окна — не замер, а его начало.
class TimingWindow {
 public:
    constexpr TimingWindow(int64_t window_us, uint32_t budget_us)
        : window_us_(window_us), budget_us_(budget_us) {}

    void add(uint32_t value_us, int64_t now_us) {
        sum_us_ += value_us;
        ++count_;
        if (value_us > peak_us_) {
            peak_us_ = value_us;
        }
        if (value_us > budget_us_) {
            ++over_budget_;
        }
        roll(now_us);
    }

    // Кадр был, но замерять в нём нечего (рендером владеет внешний источник).
    // Окно всё равно движется, и через одно окно без замеров среднее честно
    // становится нулём вместо микросекунд от пустого вызова.
    void idle(int64_t now_us) { roll(now_us); }

    void reset(int64_t now_us) {
        window_started_us_ = now_us;
        sum_us_ = 0;
        count_ = 0;
        peak_us_ = 0;
        over_budget_ = 0;
        avg_us_ = 0;
        max_us_ = 0;
    }

    uint32_t avg_us() const { return avg_us_; }
    uint32_t max_us() const { return max_us_; }
    uint32_t over_budget() const { return over_budget_; }

 private:
    void roll(int64_t now_us) {
        if (now_us - window_started_us_ < window_us_) {
            return;
        }
        avg_us_ = count_ != 0 ? static_cast<uint32_t>(sum_us_ / count_) : 0;
        max_us_ = peak_us_;
        window_started_us_ = now_us;
        sum_us_ = 0;
        count_ = 0;
        peak_us_ = 0;
    }

    int64_t window_us_;
    uint32_t budget_us_;
    int64_t window_started_us_ = 0;
    uint64_t sum_us_ = 0;
    uint32_t count_ = 0;
    uint32_t peak_us_ = 0;
    uint32_t over_budget_ = 0;
    uint32_t avg_us_ = 0;
    uint32_t max_us_ = 0;
};

// Частота событий за окно.
//
// Счётчик кадров за окно и частота — разные числа: окно закрывается на кадре,
// пришедшем уже после его конца, поэтому кадров в окне всегда чуть больше, чем
// за секунду. Отсюда и брались 63 к/с там, где реально шло 62.4. Делим на
// фактически прошедшее время.
class RateWindow {
 public:
    explicit constexpr RateWindow(int64_t window_us) : window_us_(window_us) {}

    void tick(int64_t now_us) {
        ++count_;
        roll(now_us);
    }

    // Событий нет, а время идёт: без этого частота застыла бы на последнем
    // значении навсегда.
    void roll(int64_t now_us) {
        const int64_t elapsed = now_us - window_started_us_;
        if (elapsed < window_us_) {
            return;
        }
        rate_ = static_cast<uint32_t>(
            (static_cast<int64_t>(count_) * 1000000 + elapsed / 2) / elapsed);
        window_started_us_ = now_us;
        count_ = 0;
    }

    void reset(int64_t now_us) {
        window_started_us_ = now_us;
        count_ = 0;
        rate_ = 0;
    }

    uint32_t fps() const { return rate_; }

 private:
    int64_t window_us_;
    int64_t window_started_us_ = 0;
    uint32_t count_ = 0;
    uint32_t rate_ = 0;
};

// Кольцо последних отсчётов: возраст кадра и интервалы между фиксациями.
//
// Тридцати двух хватает на половину секунды видеопотока — ровно столько, чтобы
// увидеть срыв, и достаточно мало, чтобы обход кольца стоил дешевле самого
// кадра.
class SampleRing {
 public:
    static constexpr uint8_t kCapacity = 32;

    void push(uint32_t value) {
        samples_[next_] = value;
        next_ = static_cast<uint8_t>((next_ + 1) % kCapacity);
        if (count_ < kCapacity) {
            ++count_;
        }
    }

    void clear() {
        count_ = 0;
        next_ = 0;
    }

    uint32_t avg() const {
        if (count_ == 0) {
            return 0;
        }
        uint64_t sum = 0;
        for (uint8_t i = 0; i < count_; ++i) {
            sum += samples_[i];
        }
        return static_cast<uint32_t>(sum / count_);
    }

    uint32_t max() const {
        uint32_t peak = 0;
        for (uint8_t i = 0; i < count_; ++i) {
            if (samples_[i] > peak) {
                peak = samples_[i];
            }
        }
        return peak;
    }

    // Джиттер: наибольшее отклонение отсчёта от среднего по кольцу. Ровный
    // поток даёт ноль, рваный — величину разброса в тех же единицах.
    uint32_t spread() const {
        if (count_ == 0) {
            return 0;
        }
        const uint32_t mean = avg();
        uint32_t worst = 0;
        for (uint8_t i = 0; i < count_; ++i) {
            const uint32_t diff = samples_[i] > mean ? samples_[i] - mean : mean - samples_[i];
            if (diff > worst) {
                worst = diff;
            }
        }
        return worst;
    }

 private:
    uint32_t samples_[kCapacity] = {};
    uint8_t count_ = 0;
    uint8_t next_ = 0;
};

}  // namespace ledcast
