#include "check.h"
#include "show_font.h"
#include <cstring>
#include <initializer_list>

int main() {
    using namespace ledcast::show_font;
    const char* text = u8"РВрв";
    const char* end = text + std::strlen(text);
    CHECK_EQ(utf8_next(text, end), 0x420u);
    CHECK_EQ(utf8_next(text, end), 0x412u);
    CHECK(glyph(0x420) == glyph(0x440));
    CHECK(glyph(0x412) == glyph(0x432));
    // At the user's 16 px size, both stems of О must be equally thick;
    // edge-aligned scaling used to produce a 3 px left stem and 2 px right.
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 12; ++x) {
            CHECK_EQ(pixel(glyph(0x41e), 12, 16, x, y),
                     pixel(glyph(0x41e), 12, 16, 11 - x, y));
            CHECK_EQ(pixel(glyph(0x41e), 12, 16, x, y),
                     pixel(glyph(0x41e), 12, 16, x, 15 - y));
        }
    // The stems of Р and В must remain complete at every supported size,
    // including the first visible column as a letter enters the panel.
    for (unsigned height = 7; height <= 64; ++height) {
        const unsigned width = (5 * height + 6) / 7;
        for (unsigned cp : {0x420u, 0x412u}) {
            for (unsigned y = 0; y < height; ++y) {
                CHECK(pixel(glyph(cp), width, height, 0, y));
                CHECK(!pixel(glyph(cp), width, height, -1, y));
                CHECK(!pixel(glyph(cp), width, height, width, y));
            }
            for (int origin = -static_cast<int>(width); origin <= 96; ++origin) {
                for (int screen = 0; screen < 96; ++screen) {
                    const int local = screen - origin;
                    if (local == 0) {
                        for (unsigned y = 0; y < height; ++y)
                            CHECK(pixel(glyph(cp), width, height, local, y));
                    }
                }
            }
        }
    }
    return hosttest::report("show_font");
}
