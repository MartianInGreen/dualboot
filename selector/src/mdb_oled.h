// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mdb-dualboot — minimal monochrome OLED driver for the boot selector.
//
// Drives a 128x64 SSD1306 (Heltec V4) or SH1106 (T-Beam Supreme / 1.3" panels)
// over I2C in page-addressing mode. One static 1 KB framebuffer, a 5x7 font,
// and a handful of primitives — no Adafruit_GFX, no Adafruit_BusIO, no virtual
// dispatch. Roughly 1/10th the flash footprint of the Adafruit chain.
//
// Driver kind is selected at compile time via MDB_DISPLAY_KIND:
//   0 = SSD1306, 1 = SH1106 (132-col RAM, 2-px column offset)
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace mdb_oled {

constexpr int W = 128;
constexpr int H = 64;
constexpr int FB_LEN = W * H / 8;

extern uint8_t fb[FB_LEN];

bool init(uint8_t i2c_addr);
void clear();
void show(uint8_t i2c_addr);

void px(int x, int y, bool on);
void hline(int x, int y, int w, bool on);
void rect_fill(int x, int y, int w, int h, bool on);
void rect(int x, int y, int w, int h, bool on);
// Right-pointing isoceles triangle (selection cursor).
//   x = leftmost column, y = top row, sz = total height in px.
void tri_right(int x, int y, int sz, bool on);

void glyph(int x, int y, char c, int scale, bool on);
void text(int x, int y, const char* s, int scale, bool on);
int  text_w(const char* s, int scale);

} // namespace mdb_oled
