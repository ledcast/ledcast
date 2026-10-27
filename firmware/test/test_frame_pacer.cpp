#include "check.h"
#include "frame_pacer.h"

#include <cstdint>

int main() {
    using ledcast::FramePacer;
    FramePacer pacer;
    // Ритм не пойман: ближайшая граница прохода — не дальше одного прохода.
    CHECK_EQ(pacer.next_present(2778, 3, 1000), 1000 + 2778);

    // Кадр на каждый третий проход: шаг сетки — три прохода от прошлого показа.
    pacer.presented(100000);
    CHECK_EQ(pacer.next_present(2778, 3, 100100), 100000 + 3 * 2778);
    // Опоздавший кадр встаёт на ближайшую будущую точку той же сетки, а не
    // сдвигает её.
    CHECK_EQ(pacer.next_present(2778, 3, 100000 + 3 * 2778), 100000 + 6 * 2778);
    CHECK_EQ(pacer.next_present(2778, 3, 100000 + 7 * 2778), 100000 + 9 * 2778);
    for (int64_t now = 100001; now < 200000; now += 37) {
        const int64_t present = pacer.next_present(2778, 3, now);
        CHECK(present > now);
        CHECK(present - now <= 3 * 2778);
        CHECK_EQ((present - 100000) % (3 * 2778), 0);
    }

    // Закрыть кадр можно не раньше чем за полпрохода до его границы: раньше —
    // обмен встал бы на предыдущую границу.
    CHECK_EQ(FramePacer::commit_after(2778, 108334), 108334 - 1389);

    pacer.lost();
    CHECK_EQ(pacer.next_present(4166, 2, 5000), 5000 + 4166);
    return hosttest::report("frame_pacer");
}
