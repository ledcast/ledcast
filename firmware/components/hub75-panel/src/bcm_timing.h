#pragma once
#include <cstdint>

namespace hub75_bcm {
// Controlled scanout comparison: finish a row's PWM cycle before selecting
// another address. RGB payloads and total BCM weights remain unchanged.
inline constexpr bool kInterleavedRows = false;
// LAT occurs at the END of a DMA plane. Its OE window therefore illuminates
// the previously latched plane. Plane zero illuminates the preceding row's
// last plane at full weight; all repeated high planes use the full OE window.
constexpr uint8_t oe_shift(uint8_t loading_plane, uint8_t transition) {
    return loading_plane == 0 || loading_plane > transition
        ? 0 : static_cast<uint8_t>(transition - loading_plane + 1);
}
struct OeWindow {
    uint16_t begin;
    uint16_t end;
};

// Parallel row selection must settle BEFORE LAT, not on its falling edge.
// Reserve a dark clock before the address change and another before LAT.
constexpr uint8_t control_blanking(uint8_t requested, uint8_t latch_clocks, bool parallel) {
    const uint8_t minimum = parallel ? latch_clocks + 2 : latch_clocks;
    return requested > minimum ? requested : minimum;
}
constexpr uint16_t address_switch_clock(uint16_t width, uint8_t blank) {
    return width - blank + 1;
}

// Exact half-open interval: independently rounding the two endpoints can
// turn an odd requested pulse into an even pulse. Keep both LAT guards dark.
constexpr OeWindow oe_window(uint16_t width, uint8_t blank, uint16_t requested) {
    if (width <= 2u * blank) return {0, 0};
    const uint16_t available = width - 2u * blank;
    const uint16_t count = requested < available ? requested : available;
    const uint16_t begin = blank + (available - count) / 2u;
    return {begin, static_cast<uint16_t>(begin + count)};
}

// Each span points at contiguous, existing RGB/control payloads. High planes
// visit every address before repeating, instead of dwelling on one row for
// its entire BCM period. Serial row decoders retain their row-major protocol.
template <typename Emit>
void for_each_span(uint8_t rows, uint8_t depth, uint8_t transition,
                   bool interleaved, Emit emit) {
    if (interleaved) {
        for (uint8_t row = 0; row < rows; ++row) emit(row, 0, transition + 1);
        for (uint8_t plane = transition + 1; plane < depth; ++plane)
            for (unsigned repeat = 0; repeat < (1u << (plane - transition)); ++repeat)
                for (uint8_t row = 0; row < rows; ++row) emit(row, plane, 1);
    } else {
        for (uint8_t row = 0; row < rows; ++row) {
            emit(row, 0, depth);
            for (uint8_t plane = transition + 1; plane < depth; ++plane)
                for (unsigned repeat = 1; repeat < (1u << (plane - transition)); ++repeat)
                    emit(row, plane, 1);
        }
    }
}

constexpr uint8_t illuminated_row(uint8_t row, uint8_t plane, uint8_t rows,
                                  uint8_t transition, bool interleaved) {
    return plane == 0 || (interleaved && plane > transition)
        ? static_cast<uint8_t>(row ? row - 1 : rows - 1) : row;
}

constexpr uint16_t parallel_row_address(uint8_t row, uint8_t rows, bool direct) {
    return direct ? static_cast<uint16_t>((rows == 2 ? 0x3u : 0xfu) & ~(1u << row)) : row;
}

// Number of physical rising edges seen by PCNT on the selected address bit
// in one complete cyclic BCM schedule, including the ring boundary.
inline unsigned address_rising_edges(uint8_t rows, uint8_t depth, uint8_t transition,
                                     bool interleaved, bool direct, uint16_t mask) {
    unsigned edges = 0;
    bool have_first = false, first = false, last = false;
    for_each_span(rows, depth, transition, interleaved,
        [&](uint8_t row, uint8_t plane, uint8_t planes) {
            for (uint8_t p = plane; p < plane + planes; ++p) {
                const auto lit = illuminated_row(row, p, rows, transition, interleaved);
                const bool level = (parallel_row_address(lit, rows, direct) & mask) != 0;
                if (!have_first) { first = level; have_first = true; }
                else if (!last && level) ++edges;
                last = level;
            }
        });
    if (have_first && !last && first) ++edges;
    return edges;
}
}
