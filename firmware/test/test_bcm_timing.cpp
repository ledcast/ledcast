#include "check.h"
#include "bcm_timing.h"
#include <array>
#include <vector>

static unsigned white_dark_gap(bool interleaved) {
    constexpr unsigned width = 192, rows = 4, depth = 8, transition = 3;
    struct Slot { unsigned row, plane; };
    std::vector<Slot> slots;
    hub75_bcm::for_each_span(rows, depth, transition, interleaved,
        [&](unsigned row, unsigned plane, unsigned planes) {
            for (unsigned p = plane; p < plane + planes; ++p) slots.push_back({row, p});
        });
    std::array<unsigned, rows> last_end{};
    unsigned longest = 0;
    for (unsigned i = 0; i < slots.size() * 2; ++i) {
        const auto slot = slots[i % slots.size()];
        const auto row = hub75_bcm::illuminated_row(slot.row, slot.plane, rows,
                                                   transition, interleaved);
        const auto pulse = (((width - 4) >> hub75_bcm::oe_shift(slot.plane, transition)) * 29) >> 8;
        const auto window = hub75_bcm::oe_window(width, 4, pulse);
        if (window.begin == window.end) continue;
        const unsigned begin = i * width + window.begin;
        if (i >= slots.size() && begin - last_end[row] > longest)
            longest = begin - last_end[row];
        last_end[row] = i * width + window.end;
    }
    return longest;
}

int main() {
    // A complete cyclic signal trace: blank, settle address, latch, then
    // illuminate. Include one- and three-clock latches and minimum guards.
    for (unsigned width : {32u, 192u}) {
        for (unsigned latch : {1u, 3u}) {
            for (unsigned requested = 1; requested <= 4; ++requested) {
                const auto guard = hub75_bcm::control_blanking(requested, latch, true);
                const auto window = hub75_bcm::oe_window(width, guard, width);
                unsigned previous_address = 3;
                bool previous_lit = false, previous_lat = true;
                for (unsigned row = 0; row < 4; ++row) {
                    for (unsigned x = 0; x < width; ++x) {
                        const unsigned address = x >= hub75_bcm::address_switch_clock(width, guard)
                            ? row : (row + 3) % 4;
                        const bool lit = x >= window.begin && x < window.end;
                        const bool lat = x >= width - latch;
                        if (address != previous_address) {
                            CHECK(!lit && !previous_lit);
                            CHECK(!lat && !previous_lat);
                        }
                        if (lat && !previous_lat) CHECK_EQ(address, previous_address);
                        if (lat) { CHECK(!lit); CHECK_EQ(address, row); }
                        if (lit) CHECK_EQ(address, (row + 3) % 4);
                        previous_address = address;
                        previous_lit = lit;
                        previous_lat = lat;
                    }
                }
                CHECK_EQ(previous_address, 3);
            }
        }
    }
    for (unsigned rows : {2u, 4u, 8u, 16u, 32u}) {
        for (unsigned depth = 1; depth <= 8; ++depth) {
            for (unsigned transition = 0; transition < depth; ++transition) {
                unsigned sweeps = 1;
                for (unsigned plane = transition + 1; plane < depth; ++plane)
                    sweeps += 1u << (plane - transition);
                CHECK_EQ(hub75_bcm::address_rising_edges(rows, depth, transition,
                             false, false, rows / 2), 1);
                CHECK_EQ(hub75_bcm::address_rising_edges(rows, depth, transition,
                             true, false, rows / 2), sweeps);
                if (rows <= 4)
                    CHECK_EQ(hub75_bcm::address_rising_edges(rows, depth, transition,
                                 true, true, rows / 2), sweeps);
            }
        }
    }
    for (bool interleaved : {false, true}) {
        for (unsigned rows : {1u, 4u, 16u}) {
            for (unsigned depth = 1; depth <= 8; ++depth) {
                for (unsigned transition = 0; transition < depth; ++transition) {
                    std::array<std::array<unsigned, 8>, 16> weights{};
                    unsigned previous_row = rows - 1, previous_plane = depth - 1;
                    unsigned slots = 0;
                    hub75_bcm::for_each_span(rows, depth, transition, interleaved,
                        [&](unsigned row, unsigned first_plane, unsigned planes) {
                            CHECK(first_plane + planes <= depth);
                            for (unsigned plane = first_plane; plane < first_plane + planes; ++plane) {
                                CHECK_EQ(hub75_bcm::illuminated_row(row, plane, rows,
                                             transition, interleaved), previous_row);
                                weights[previous_row][previous_plane] +=
                                    (1u << transition) >> hub75_bcm::oe_shift(plane, transition);
                                previous_row = row;
                                previous_plane = plane;
                                ++slots;
                            }
                        });
                    CHECK_EQ(previous_row, rows - 1);
                    CHECK_EQ(previous_plane, depth - 1);
                    unsigned expected_slots = depth;
                    for (unsigned plane = transition + 1; plane < depth; ++plane)
                        expected_slots += (1u << (plane - transition)) - 1;
                    CHECK_EQ(slots, expected_slots * rows);
                    for (unsigned row = 0; row < rows; ++row)
                        for (unsigned plane = 0; plane < depth; ++plane)
                            CHECK_EQ(weights[row][plane], 1u << plane);
                }
            }
        }
    }
    const auto old_gap = white_dark_gap(false), new_gap = white_dark_gap(true);
    std::printf("White maximum dark gap at 10 MHz: row-major %.1f us, interleaved %.1f us\n",
                old_gap / 10.0, new_gap / 10.0);
    CHECK(new_gap < old_gap / 4);
    for (unsigned width : {31u, 32u, 64u, 192u, 384u}) {
        for (unsigned blank = 1; blank <= 4; ++blank) {
            for (unsigned requested = 0; requested <= width; ++requested) {
                const auto window = hub75_bcm::oe_window(width, blank, requested);
                const unsigned available = width - 2u * blank;
                const unsigned expected = requested < available ? requested : available;
                unsigned lit = 0;
                for (unsigned x = 0; x < width; ++x)
                    if (x >= window.begin && x < window.end) ++lit;
                CHECK_EQ(lit, expected);
                CHECK(window.begin >= blank);
                CHECK(window.end <= width - blank);
                const unsigned left = window.begin - blank;
                const unsigned right = width - blank - window.end;
                CHECK(right == left || right == left + 1);
            }
        }
    }
    CHECK_EQ(hub75_bcm::oe_window(192, 4, 21).end -
             hub75_bcm::oe_window(192, 4, 21).begin, 21);
    CHECK_EQ(hub75_bcm::oe_window(4, 4, 4).end, 0);
    // Simulate a cyclic HUB75 shift/LAT pipeline, not merely the helper's
    // return values. OE contributes to the PREVIOUS latched plane. Integer
    // units represent fractional low-bit windows without rounding noise.
    for (unsigned depth = 2; depth <= 8; ++depth) {
        for (unsigned transition = 0; transition < depth; ++transition) {
            std::array<unsigned, 8> weights{};
            unsigned latched = depth - 1;
            auto shift_and_latch = [&](unsigned plane) {
                weights[latched] += (1u << transition) >>
                    hub75_bcm::oe_shift(plane, transition);
                latched = plane;
            };
            for (unsigned plane = 0; plane < depth; ++plane) shift_and_latch(plane);
            for (unsigned plane = transition + 1; plane < depth; ++plane)
                for (unsigned repeat = 1; repeat < (1u << (plane - transition)); ++repeat)
                    shift_and_latch(plane);
            for (unsigned plane = 0; plane < depth; ++plane)
                CHECK_EQ(weights[plane], 1u << plane);
            // Every possible pixel code must preserve binary ordering.
            unsigned previous = 0;
            for (unsigned code = 0; code < (1u << depth); ++code) {
                unsigned light = 0;
                for (unsigned plane = 0; plane < depth; ++plane)
                    if (code & (1u << plane)) light += weights[plane];
                CHECK_EQ(light, code);
                if (code) CHECK(light > previous);
                previous = light;
            }
        }
    }
    return hosttest::report("bcm_timing");
}
