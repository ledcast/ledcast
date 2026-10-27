#include "check.h"
#include "flip_wait_timing.h"

using namespace hub75_flip;

namespace {

// 22 модуля по 64 электрических точки, 4 адресных строки, 8 разрядов, один
// проход кольца без повторов старших планов: 1408 * 4 * 8 слов по два байта.
constexpr uint32_t kRingBytes = 1408u * 4u * 8u * 2u;
constexpr uint32_t kPclk20M = 22857142u;  // 160 МГц / 7 — то, что выходит на «20 МГц»

void test_ring_period_counts_two_bytes_per_bus_clock() {
    // 90112 байт это 45056 слов, на 22,857 МГц ровно 1971 мкс.
    CHECK_EQ(ring_period_us(kRingBytes, kPclk20M), 1971u);
}

void test_ring_period_scales_with_bus_clock() {
    CHECK_EQ(ring_period_us(2000u, 1000000u), 1000u);
    CHECK_EQ(ring_period_us(2000u, 2000000u), 500u);
}

void test_ring_period_is_zero_without_data_or_clock() {
    CHECK_EQ(ring_period_us(0u, kPclk20M), 0u);
    CHECK_EQ(ring_period_us(kRingBytes, 0u), 0u);
}

void test_timeout_covers_three_ring_passes() {
    // Обмен вступает в силу не позже второго прохода кольца, третий — запас на
    // задержку прерывания и планировщика.
    CHECK_EQ(wait_timeout_us(kRingBytes, kPclk20M), 3u * 1971u);
}

void test_timeout_has_a_floor_for_tiny_rings() {
    CHECK_EQ(wait_timeout_us(2000u, 20000000u), kMinWaitTimeoutUs);
}

void test_timeout_has_a_ceiling_and_never_hangs() {
    CHECK_EQ(wait_timeout_us(0xFFFFFFFFu, 1000u), kMaxWaitTimeoutUs);
}

void test_address_inside_the_chain_is_recognised() {
    constexpr uintptr_t base = 0x3FC80000u;
    constexpr uint32_t count = 32u;
    constexpr uint32_t desc = 12u;
    CHECK(addr_in_chain(base, base, count, desc));
    CHECK(addr_in_chain(base + desc, base, count, desc));
    CHECK(addr_in_chain(base + (count - 1u) * desc, base, count, desc));
}

void test_address_outside_the_chain_is_rejected() {
    constexpr uintptr_t base = 0x3FC80000u;
    constexpr uint32_t count = 32u;
    constexpr uint32_t desc = 12u;
    CHECK(!addr_in_chain(base - 1u, base, count, desc));
    CHECK(!addr_in_chain(base + count * desc, base, count, desc));
    CHECK(!addr_in_chain(0u, base, count, desc));
}

}  // namespace

int main() {
    test_ring_period_counts_two_bytes_per_bus_clock();
    test_ring_period_scales_with_bus_clock();
    test_ring_period_is_zero_without_data_or_clock();
    test_timeout_covers_three_ring_passes();
    test_timeout_has_a_floor_for_tiny_rings();
    test_timeout_has_a_ceiling_and_never_hangs();
    test_address_inside_the_chain_is_recognised();
    test_address_outside_the_chain_is_rejected();
    return hosttest::report("flip_wait");
}
