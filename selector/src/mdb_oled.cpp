// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mdb-dualboot — minimal OLED driver implementation. See mdb_oled.h.
#include "mdb_oled.h"
#include "mdb_font5x7.h"

#include <Wire.h>
#include <string.h>

#ifndef MDB_DISPLAY_KIND
#error "MDB_DISPLAY_KIND must be defined: 0 = SSD1306, 1 = SH1106"
#endif

namespace mdb_oled {

uint8_t fb[FB_LEN];

// SSD1306 datasheet 8.1.5.2 / SH1106 datasheet 8.1: control byte selects the
// type of stream that follows on I2C.
//   0x00 — Co=0, D/C#=0 → command stream
//   0x40 — Co=0, D/C#=1 → data stream
static constexpr uint8_t CTRL_CMD  = 0x00;
static constexpr uint8_t CTRL_DATA = 0x40;

#if MDB_DISPLAY_KIND == 0
// SSD1306: enable internal charge pump, page-addressing mode.
static const uint8_t init_seq[] = {
  0xAE,             // display off
  0xD5, 0x80,       // clock divide
  0xA8, 0x3F,       // multiplex 1/64
  0xD3, 0x00,       // display offset 0
  0x40,             // start line 0
  0x8D, 0x14,       // charge pump on
  0x20, 0x02,       // page-addressing mode
  0xA1,             // segment remap (col 127→SEG0)
  0xC8,             // COM scan reverse
  0xDA, 0x12,       // COM pins config
  0x81, 0xCF,       // contrast
  0xD9, 0xF1,       // pre-charge
  0xDB, 0x40,       // VCOMH deselect
  0xA4,             // resume from RAM
  0xA6,             // not inverted
  0xAF,             // display on
};
static constexpr int COL_OFFSET = 0;
#elif MDB_DISPLAY_KIND == 1
// SH1106: built-in DC-DC, page-addressing only, 132-col RAM (skip 2 leading).
static const uint8_t init_seq[] = {
  0xAE,
  0xD5, 0x80,
  0xA8, 0x3F,
  0xD3, 0x00,
  0x40,
  0xAD, 0x8B,       // DC-DC enable (SH1106-specific)
  0xA1,
  0xC8,
  0xDA, 0x12,
  0x81, 0x80,
  0xD9, 0x22,
  0xDB, 0x35,
  0xA4,
  0xA6,
  0xAF,
};
static constexpr int COL_OFFSET = 2;
#else
#error "Unsupported MDB_DISPLAY_KIND"
#endif

static bool send_cmds(uint8_t addr, const uint8_t* cmds, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(CTRL_CMD);
  Wire.write(cmds, n);
  return Wire.endTransmission() == 0;
}

bool init(uint8_t i2c_addr) {
  Wire.beginTransmission(i2c_addr);
  if (Wire.endTransmission() != 0) return false;
  if (!send_cmds(i2c_addr, init_seq, sizeof(init_seq))) return false;
  clear();
  show(i2c_addr);
  return true;
}

void clear() { memset(fb, 0, FB_LEN); }

void show(uint8_t i2c_addr) {
  // 8 pages × 128 columns. Wire's default ESP32 buffer is 128 B, so we ship
  // 64-byte halves to leave room for the control byte.
  for (int page = 0; page < 8; page++) {
    uint8_t hdr[3] = {
      static_cast<uint8_t>(0xB0 | page),
      static_cast<uint8_t>(0x00 | (COL_OFFSET & 0x0F)),
      static_cast<uint8_t>(0x10 | ((COL_OFFSET >> 4) & 0x0F)),
    };
    send_cmds(i2c_addr, hdr, 3);
    const uint8_t* row = fb + page * W;
    for (int chunk = 0; chunk < W; chunk += 64) {
      Wire.beginTransmission(i2c_addr);
      Wire.write(CTRL_DATA);
      Wire.write(row + chunk, 64);
      Wire.endTransmission();
    }
  }
}

void px(int x, int y, bool on) {
  if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
  uint8_t* p = &fb[(y >> 3) * W + x];
  uint8_t mask = uint8_t(1) << (y & 7);
  if (on) *p |= mask;
  else    *p &= ~mask;
}

void hline(int x, int y, int w, bool on) {
  if ((unsigned)y >= (unsigned)H) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > W) w = W - x;
  if (w <= 0) return;
  uint8_t mask = uint8_t(1) << (y & 7);
  uint8_t* p = &fb[(y >> 3) * W + x];
  if (on) {
    for (int i = 0; i < w; i++) p[i] |= mask;
  } else {
    uint8_t inv = ~mask;
    for (int i = 0; i < w; i++) p[i] &= inv;
  }
}

void rect_fill(int x, int y, int w, int h, bool on) {
  for (int yi = 0; yi < h; yi++) hline(x, y + yi, w, on);
}

void rect(int x, int y, int w, int h, bool on) {
  if (w <= 0 || h <= 0) return;
  hline(x, y, w, on);
  hline(x, y + h - 1, w, on);
  for (int yi = 1; yi < h - 1; yi++) {
    px(x, y + yi, on);
    px(x + w - 1, y + yi, on);
  }
}

void tri_right(int x, int y, int sz, bool on) {
  // Solid right-pointing isoceles triangle. Each row from top/bottom
  // narrows toward the middle; widest row at sz/2 spans sz/2 + 1 px.
  int half = sz / 2;
  for (int row = 0; row < sz; row++) {
    int dist = row < half ? half - row : row - half;
    int w = half - dist + 1;
    hline(x, y + row, w, on);
  }
}

void glyph(int x, int y, char c, int scale, bool on) {
  if ((unsigned char)c < FONT_FIRST || (unsigned char)c > FONT_LAST) c = '?';
  const uint8_t* g = &font5x7[((unsigned char)c - FONT_FIRST) * FONT_BYTES];
  for (int col = 0; col < FONT_W; col++) {
    uint8_t bits = g[col];
    for (int row = 0; row < 8; row++) {
      if (!(bits & (uint8_t(1) << row))) continue;
      if (scale <= 1) px(x + col, y + row, on);
      else            rect_fill(x + col * scale, y + row * scale, scale, scale, on);
    }
  }
}

void text(int x, int y, const char* s, int scale, bool on) {
  if (!s) return;
  int adv = (FONT_W + 1) * (scale > 0 ? scale : 1);
  for (; *s; s++, x += adv) glyph(x, y, *s, scale, on);
}

int text_w(const char* s, int scale) {
  if (!s) return 0;
  int n = (int)strlen(s);
  return n * (FONT_W + 1) * (scale > 0 ? scale : 1);
}

} // namespace mdb_oled
