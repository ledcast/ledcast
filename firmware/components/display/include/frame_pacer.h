#pragma once

#include <cstdint>

namespace ledcast {

// Сетка кадров вывода — общая для видеопотока и автономного показа.
//
// Кадр становится виден на границе прохода развёртки, и граница эта — каждый
// passes_per_frame-й проход (DisplayScanout): ровно 1/frame_rate секунды.
// Источник спрашивает, когда станет виден следующий кадр, готовит картинку на
// этот момент, закрывает кадр не раньше commit_after() и сообщает, когда обмен
// подтвердился. Так шаг движения строго равномерный, а время показа известно
// заранее, а не после того, как кадр уже нарисован.
//
// Без ESP-IDF: проверяется на хосте.
class FramePacer {
  public:
    // Момент показа следующего кадра: шаг сетки от прошлого показа. Опоздавший
    // кадр встаёт на ближайшую будущую точку той же сетки, а не сдвигает её.
    // Пока ритм не пойман, ближайшая граница прохода — не дальше одного прохода.
    int64_t next_present(uint32_t pass_us, uint8_t passes_per_frame, int64_t now_us) const {
        if (anchor_us_ == 0 || pass_us == 0) return now_us + pass_us;
        const int64_t step = static_cast<int64_t>(pass_us) * (passes_per_frame != 0 ? passes_per_frame : 1);
        int64_t present = anchor_us_ + step;
        if (present <= now_us) present += ((now_us - present) / step + 1) * step;
        return present;
    }

    // Обмен копий встаёт на первую границу прохода после закрытия кадра.
    // Закрыть кадр раньше чем за полпрохода до нужной границы — значит
    // показать его на проход раньше.
    static int64_t commit_after(uint32_t pass_us, int64_t present_us) {
        return present_us - static_cast<int64_t>(pass_us / 2);
    }

    // Кадр встал на панель в момент vsync_us (или, при одной копии кадра, в
    // назначенный момент показа). От него отсчитывается следующий шаг.
    void presented(int64_t vsync_us) { anchor_us_ = vsync_us; }

    // Ритм потерян: вывод пересобран, обмен не подтвердился.
    void lost() { anchor_us_ = 0; }

  private:
    int64_t anchor_us_ = 0;
};

}  // namespace ledcast
