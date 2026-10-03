// coachglyph.h - the 5 x 7 block font Coach draws its words in: on the line (coachmark.h, as quads in the
// telemetry world) and on the HUD (coachhud.h, as screen quads). Pure data, no dependencies.
#pragma once

#include <cstdint>

namespace coachglyph {

/// Seven rows of five bits, the leftmost column the highest bit. Null for a character it lacks.
inline const uint8_t* Glyph(char c) {
    static const uint8_t az[26][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},  // A B C
        {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},  // D E F
        {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},      // G H I
        {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},  // J K L
        {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},  // M N O
        {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},  // P Q R
        {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},  // S T U
        {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},   // V W X
        {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31}};                                   // Y Z
    static const uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},   {14, 17, 1, 2, 4, 8, 31},  {31, 2, 4, 2, 1, 17, 14},
        {2, 6, 10, 18, 31, 2, 2},     {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}};
    static const uint8_t dash[7] = {0, 0, 0, 31, 0, 0, 0}, slash[7] = {1, 1, 2, 4, 8, 16, 16},
                         dot[7] = {0, 0, 0, 0, 0, 12, 12}, bang[7] = {4, 4, 4, 4, 4, 0, 4}, tick[7] = {4, 4, 8, 0, 0, 0, 0},
                         space[7] = {0, 0, 0, 0, 0, 0, 0};
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return az[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
        case '-': return dash;
        case '/': return slash;
        case '.': return dot;
        case '!': return bang;
        case '\'': return tick;
        case ' ': return space;
        default: return nullptr;
    }
}

/// Columns a character advances by: five lit, one gap.
constexpr int kAdvance = 6;

}  // namespace coachglyph
