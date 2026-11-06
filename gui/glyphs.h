#pragma once

#include <cstdint>

// 5x7 bitmap glyphs for button labels. Each row holds 5 bits, MSB = leftmost.
// Supported: 0-9 * # M E N U X I T P S. Returns false for anything else.
bool glyph_rows(char c, uint8_t rows[7]);
