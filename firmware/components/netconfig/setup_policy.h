#pragma once
#include <cstdint>

namespace ledcast::netconfig {
// A phone configuring the AP owns the radio. Background STA attempts must not
// take it off-channel; an explicit profile selection is the only exception.
constexpr bool may_retry(bool connected, bool connecting, bool scanning,
                         unsigned clients, int64_t now, int64_t deadline) {
    return !connected && !connecting && !scanning && clients == 0 &&
           deadline != 0 && now >= deadline;
}
constexpr int64_t retry_delay_us(unsigned failures) {
    return failures < 2 ? 15000000 : failures < 4 ? 30000000 : 60000000;
}
} // namespace ledcast::netconfig
