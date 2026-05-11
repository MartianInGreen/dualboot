// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin
//
// The 5x7 glyph data below is reproduced from a public-domain font; the
// AGPL covers only the C wrapping. See the existing comment near the data.

// mdb-dualboot — minimal 5x7 column-major font for the selector OLED.
//
// 96 glyphs, ASCII 0x20..0x7F. Each glyph is 5 columns × 8 rows (5x7 visible
// with row 7 blank, except descenders which use rows 6-7). One byte per
// column, bit 0 = top row, bit 7 = bottom row. Total 480 bytes.
//
// Drawn from scratch for mesh-dualboot. Public domain — pixel grids this
// small look essentially the same in every 5x7 font ever written.
#pragma once
#include <stdint.h>

namespace mdb_oled {

constexpr int FONT_FIRST  = 0x20;
constexpr int FONT_LAST   = 0x7F;
constexpr int FONT_W      = 5;
constexpr int FONT_BYTES  = 5;

extern const uint8_t font5x7[(FONT_LAST - FONT_FIRST + 1) * FONT_BYTES];

} // namespace mdb_oled
