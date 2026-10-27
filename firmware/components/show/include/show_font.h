#pragma once
#include <cstdint>

namespace ledcast::show_font {
// Sans 5x7 uppercase font. Scaling supplies the weight; glyphs are not dilated,
// because doing so closes counters and changes Cyrillic letterforms.
constexpr uint8_t kDigits[10][7] = {
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},
    {31,16,16,30,1,1,30},{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}
};
constexpr uint8_t kLatin[26][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};
// А..Я, with Ё handled separately. Lowercase is folded to uppercase.
constexpr uint8_t kCyrillic[32][7] = {
    {14,17,17,31,17,17,17},{31,16,16,30,17,17,30},{30,17,17,30,17,17,30},{31,16,16,16,16,16,16},
    {14,10,10,10,10,31,17},{31,16,16,30,16,16,31},{21,21,14,4,14,21,21},{14,17,1,6,1,17,14},
    {17,17,19,21,25,17,17},{10,4,17,19,21,25,17},{17,18,20,24,20,18,17},{7,9,17,17,17,17,17},
    {17,27,21,21,17,17,17},{17,17,17,31,17,17,17},{14,17,17,17,17,17,14},{31,17,17,17,17,17,17},
    {30,17,17,30,16,16,16},{14,17,16,16,16,17,14},{31,4,4,4,4,4,4},{17,17,17,15,1,17,14},
    {4,14,21,21,21,14,4},{17,17,10,4,10,17,17},{17,17,17,17,17,31,1},{17,17,17,15,1,1,1},
    {21,21,21,21,21,21,31},{21,21,21,21,21,31,1},{24,8,8,14,9,9,14},{17,17,17,29,19,19,29},
    {16,16,16,30,17,17,30},{17,17,19,21,19,17,17},{7,9,9,15,9,9,7},{15,17,17,15,5,9,17}
};
constexpr uint8_t kQuestion[7] = {14,17,1,2,4,0,4};
constexpr uint8_t kYo[7] = {10,0,31,16,30,16,31};
constexpr uint8_t kDash[7] = {0,0,0,31,0,0,0};
constexpr uint8_t kDot[7] = {0,0,0,0,0,12,12};
constexpr uint8_t kColon[7] = {0,12,12,0,12,12,0};
constexpr uint8_t kBang[7] = {4,4,4,4,4,0,4};
constexpr uint8_t kPercent[7] = {24,25,2,4,8,19,3};
constexpr uint8_t kSpace[7] = {};

inline uint32_t utf8_next(const char*& cursor, const char* end) {
    if (cursor >= end) return 0;
    const uint8_t c0 = static_cast<uint8_t>(*cursor++);
    if (c0 < 0x80) return c0;
    auto continuation = [&](uint8_t& out) {
        if (cursor >= end || (static_cast<uint8_t>(*cursor) & 0xc0u) != 0x80u) return false;
        out = static_cast<uint8_t>(*cursor++ & 0x3f); return true;
    };
    uint8_t c1 = 0, c2 = 0;
    if ((c0 & 0xe0u) == 0xc0u && continuation(c1)) return ((c0 & 0x1fu) << 6) | c1;
    if ((c0 & 0xf0u) == 0xe0u && continuation(c1) && continuation(c2)) return ((c0 & 0x0fu) << 12) | (c1 << 6) | c2;
    return '?';
}

inline const uint8_t* glyph(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') cp -= 'a' - 'A';
    if (cp >= '0' && cp <= '9') return kDigits[cp - '0'];
    if (cp >= 'A' && cp <= 'Z') return kLatin[cp - 'A'];
    if (cp >= 0x430 && cp <= 0x44f) cp -= 0x20;
    if (cp == 0x401 || cp == 0x451) return kYo;
    if (cp >= 0x410 && cp <= 0x42f) return kCyrillic[cp - 0x410];
    if (cp == ' ') return kSpace;
    if (cp == '-' || cp == 0x2013 || cp == 0x2014) return kDash;
    if (cp == '.') return kDot;
    if (cp == ':') return kColon;
    if (cp == '!') return kBang;
    if (cp == '%') return kPercent;
    return kQuestion;
}
// Integer-grid rasterization preserves complete columns during translation.
inline bool pixel(const uint8_t* rows, unsigned width, unsigned height, int x, int y) {
    if (x < 0 || y < 0 || static_cast<unsigned>(x) >= width || static_cast<unsigned>(y) >= height) return false;
    // Sample LED centres, not their left/top edges. Edge sampling at 16 px
    // gives the two outer stems different widths (3 vs 2 pixels).
    const unsigned column = (2u * static_cast<unsigned>(x) + 1u) * 5u / (2u * width);
    const unsigned row = (2u * static_cast<unsigned>(y) + 1u) * 7u / (2u * height);
    return (rows[row] & (1u << (4 - column))) != 0;
}
} // namespace ledcast::show_font
