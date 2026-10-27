#include "check.h"
#include "show_blend.h"

#include <cstdint>
#include <cstdlib>

int main() {
    using ledcast::colour::kGamma16;
    using ledcast::show_blend::dimmed;
    for (unsigned value = 0; value < 256; ++value) {
        CHECK_EQ(dimmed(static_cast<uint8_t>(value), 256), value);
        CHECK_EQ(dimmed(static_cast<uint8_t>(value), 0), 0u);
        for (unsigned share = 0; share <= 256; ++share) {
            const uint8_t code = dimmed(static_cast<uint8_t>(value), share);
            CHECK(code <= value);
            // Ближайший код к нужной линейной яркости.
            const long target = (static_cast<long>(kGamma16[value]) * share + 128) >> 8;
            const long error = std::labs(kGamma16[code] - target);
            if (code > 0) CHECK(error <= std::labs(kGamma16[code - 1] - target));
            if (code < value) CHECK(error <= std::labs(kGamma16[code + 1] - target));
            // Точка между двумя столбцами светит в сумме как целая: доли
            // складываются в линейном свете, а не в кодах.
            const long sum = kGamma16[code] + kGamma16[dimmed(static_cast<uint8_t>(value), 256 - share)];
            CHECK(std::labs(sum - static_cast<long>(kGamma16[value])) <= 600);
        }
    }
    return hosttest::report("show_blend");
}
