// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mesh-dualboot selector — clean-room implementation.
// Basis: ESP-IDF OTA API documentation and the Apache-2.0 reference at
// https://github.com/georgik/esp32-graphical-bootloader.
//
// Per-board configuration is supplied entirely via -D macros in
// platformio.ini. The display driver is chosen at compile time
// (mdb_oled handles SSD1306 via MDB_DISPLAY_KIND=0 and SH1106 via =1).

#include <Arduino.h>
#include <Wire.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/rtc_io.h>

#include "mdb_oled.h"
#include "mdb_pref.h"

#if defined(MDB_HAS_AXP2101) && MDB_HAS_AXP2101
  #include <XPowersLib.h>
#endif

#define SCREEN_WIDTH    mdb_oled::W
#define SCREEN_HEIGHT   mdb_oled::H
#define DEBOUNCE_MS     30
#define LONG_PRESS_MS   500

// Slot labels come from slots.json via slots_build.py → build flags. Defaults
// preserve the original behavior if the pre-script didn't run.
#ifndef MDB_SLOT0_NAME
#define MDB_SLOT0_NAME "MeshCore"
#endif
#ifndef MDB_SLOT1_NAME
#define MDB_SLOT1_NAME "Meshtastic"
#endif

enum Slot : uint8_t {
  SLOT_0 = 0,
  SLOT_1 = 1,
  N_SLOTS
};

static const char* slot_name(Slot s) {
  switch (s) {
    case SLOT_0: return MDB_SLOT0_NAME;
    case SLOT_1: return MDB_SLOT1_NAME;
    default:     return "?";
  }
}

static esp_partition_subtype_t slot_subtype(Slot s) {
  return s == SLOT_0
    ? ESP_PARTITION_SUBTYPE_APP_OTA_0
    : ESP_PARTITION_SUBTYPE_APP_OTA_1;
}

#ifndef MDB_PIN_OLED_RESET
#define MDB_PIN_OLED_RESET -1
#endif

static Slot current = SLOT_0;
static bool oled_ok = false;
static bool auto_boot_cancelled = false;
static unsigned long auto_boot_start_ms = 0;

#if defined(MDB_HAS_AXP2101) && MDB_HAS_AXP2101
static XPowersAXP2101 pmu;
static bool init_pmu() {
  // T-Beam Supreme: AXP2101 sits on Wire1 (PMU bus). DCDC1 powers the
  // OLED; needs to be driven to 3.3 V and enabled before the OLED
  // responds on the main I2C bus.
  Wire1.begin(MDB_PIN_PMU_SDA, MDB_PIN_PMU_SCL);
  Wire1.setClock(100000);
  if (!pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, MDB_PIN_PMU_SDA, MDB_PIN_PMU_SCL)) {
    return false;
  }
  pmu.setDC1Voltage(3300);
  pmu.enableDC1();
  delay(50);
  return true;
}
#endif

volatile int g_phase = 0;
volatile bool oled_ok_for_led = false;
static volatile int g_i2c_count = -1;
static volatile bool g_booting = false;
// After a failed boot attempt, require the user to release PRG before any
// further long-press will re-fire. Without this, a user holding PRG
// through the 3-second error display gets the same boot tried again on
// the next loop iteration — and again, and again.
static bool g_boot_failed_pending_release = false;
// "User has touched PRG since power-on." Used to keep the late-OLED-init
// path from clobbering an explicit cancellation the user already made
// during LED-only mode.
static bool g_user_input_seen = false;

#ifdef MDB_PIN_LED
static void led_set(int level) { digitalWrite(MDB_PIN_LED, level ? HIGH : LOW); }

static void led_pulse(int on_ms, int off_ms) {
  led_set(1); vTaskDelay(pdMS_TO_TICKS(on_ms));
  led_set(0); vTaskDelay(pdMS_TO_TICKS(off_ms));
}

// LED indicator: in normal (LED-only) operation, blinks the CURRENT
// selection — 1 quick blink for MeshCore, 2 for Meshtastic — every ~2 s.
// During the brief setup window or in OLED mode it falls back to phase
// counting so we still get diagnostics.
extern volatile int g_phase;
extern volatile bool oled_ok_for_led;
static volatile int g_selection_for_led = 0;  // 0=MeshCore, 1=Meshtastic

static void led_task(void*) {
  pinMode(MDB_PIN_LED, OUTPUT);
  for (;;) {
    if (g_booting) { led_set(1); vTaskDelay(pdMS_TO_TICKS(200)); continue; }

    if (g_phase >= 5) {
      // Setup is done. Indicate current selection.
      int n = g_selection_for_led + 1;  // 1 or 2 quick blinks
      for (int i = 0; i < n; i++) led_pulse(80, 220);
      vTaskDelay(pdMS_TO_TICKS(2000));
    } else {
      // Setup still in progress — diagnostic phase counter.
      led_pulse(700, 350);
      int p = g_phase;
      for (int i = 0; i < p; i++) led_pulse(120, 250);
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
  }
}

static void led_start_indicator() {
  xTaskCreate(led_task, "mdb_led", 2048, nullptr, 1, nullptr);
}
#else
static void led_start_indicator() {}
#endif

static bool init_oled() {
  if (!mdb_oled::init(MDB_OLED_ADDR)) return false;
  // Confirm OLED is on the bus at this point.
  Wire.beginTransmission(MDB_OLED_ADDR);
  if (Wire.endTransmission() != 0) return false;
  return true;
}

static void draw_serial(int countdown_ms) {
  char tail[24];
  if (auto_boot_cancelled) {
    strcpy(tail, "(hold PRG to boot)");
  } else {
    int sec = (countdown_ms + 999) / 1000;
    snprintf(tail, sizeof(tail), "auto in %ds", sec);
  }
  Serial.printf("[selector] %s%s   %s%s   %s\n",
    current == SLOT_0 ? "> " : "  ", slot_name(SLOT_0),
    current == SLOT_1 ? "> " : "  ", slot_name(SLOT_1),
    tail);
}

// ── Layout (128x64) ──
//  0..11   header bar: "mesh-dualboot", inverted, centered
// 12..29   slot row 0
// 30..47   slot row 1
// 48..51   gap
// 52..63   footer: progress bar / hint text

static void draw_header() {
  mdb_oled::rect_fill(0, 0, SCREEN_WIDTH, 12, true);
  static const char title[] = "mesh-dualboot";
  int tx = (SCREEN_WIDTH - mdb_oled::text_w(title, 1)) / 2;
  mdb_oled::text(tx, 2, title, 1, false);
}

// Choose the largest text size at which every slot label fits within the
// available row width. Computed once, cached.
static int slot_text_size() {
  static int cached = 0;
  if (cached) return cached;
  const int avail = SCREEN_WIDTH - 16;  // 16px reserved for pointer
  for (int s = 2; s >= 1; s--) {
    int max_w = 0;
    for (int i = 0; i < N_SLOTS; i++) {
      int w = mdb_oled::text_w(slot_name((Slot)i), s);
      if (w > max_w) max_w = w;
    }
    if (max_w <= avail) { cached = s; return cached; }
  }
  cached = 1;
  return cached;
}

static void draw_slot_row(int idx, int y) {
  bool sel = (idx == (int)current);
  if (sel) {
    mdb_oled::rect_fill(0, y, SCREEN_WIDTH, 18, true);
    mdb_oled::tri_right(4, y + 4, 10, false);
  }
  int size = slot_text_size();
  int char_h = 8 * size;
  mdb_oled::text(16, y + (18 - char_h) / 2, slot_name((Slot)idx), size, !sel);
}

// Render footer in one of three modes:
//   hold_ms > 0  → "boot in: [████   ]" filling toward LONG_PRESS_MS
//   countdown_ms > 0 → auto-boot countdown bar draining over MDB_AUTO_BOOT_MS
//   neither      → static "tap:cycle  hold:boot" hint
static void draw_footer(int countdown_ms, int hold_ms) {
  if (hold_ms > 0) {
    int bar_x = 4, bar_y = SCREEN_HEIGHT - 10, bar_w = 80, bar_h = 8;
    mdb_oled::rect(bar_x, bar_y, bar_w, bar_h, true);
    int fill_w = (hold_ms * (bar_w - 2)) / LONG_PRESS_MS;
    if (fill_w > bar_w - 2) fill_w = bar_w - 2;
    if (fill_w > 0) mdb_oled::rect_fill(bar_x + 1, bar_y + 1, fill_w, bar_h - 2, true);
    mdb_oled::text(SCREEN_WIDTH - 28, SCREEN_HEIGHT - 8, "hold", 1, true);
  } else if (auto_boot_cancelled) {
    static const char hint[] = "tap:cycle  hold:boot";
    int tx = (SCREEN_WIDTH - mdb_oled::text_w(hint, 1)) / 2;
    mdb_oled::text(tx, SCREEN_HEIGHT - 8, hint, 1, true);
  } else {
    int bar_x = 4, bar_y = SCREEN_HEIGHT - 10, bar_w = 80, bar_h = 8;
    mdb_oled::rect(bar_x, bar_y, bar_w, bar_h, true);
    int fill_w = (countdown_ms * (bar_w - 2)) / MDB_AUTO_BOOT_MS;
    if (fill_w > 0) mdb_oled::rect_fill(bar_x + 1, bar_y + 1, fill_w, bar_h - 2, true);
    char buf[8];
    int sec = (countdown_ms + 999) / 1000;
    snprintf(buf, sizeof(buf), "%ds", sec);
    mdb_oled::text(SCREEN_WIDTH - 16, SCREEN_HEIGHT - 8, buf, 1, true);
  }
}

static void draw_menu(int countdown_ms, int hold_ms = 0) {
  if (!oled_ok) { draw_serial(countdown_ms); return; }
  mdb_oled::clear();
  draw_header();
  draw_slot_row(0, 12);
  draw_slot_row(1, 30);
  draw_footer(countdown_ms, hold_ms);
  mdb_oled::show(MDB_OLED_ADDR);
}

static void draw_loading(Slot s) {
  if (!oled_ok) return;
  mdb_oled::clear();
  draw_header();
  mdb_oled::text(0, 22, "  Loading...", 1, true);
  int nx = (SCREEN_WIDTH - mdb_oled::text_w(slot_name(s), 2)) / 2;
  mdb_oled::text(nx < 0 ? 0 : nx, 36, slot_name(s), 2, true);
  mdb_oled::show(MDB_OLED_ADDR);
}

static void draw_splash() {
  if (!oled_ok) return;
  mdb_oled::clear();
  static const char w1[] = "mesh";
  static const char w2[] = "dualboot";
  int x1 = (SCREEN_WIDTH - mdb_oled::text_w(w1, 2)) / 2;
  int x2 = (SCREEN_WIDTH - mdb_oled::text_w(w2, 2)) / 2;
  mdb_oled::text(x1, 14, w1, 2, true);
  mdb_oled::text(x2, 36, w2, 2, true);
  // small underline accent
  mdb_oled::hline(20, 56, SCREEN_WIDTH - 40, true);
  mdb_oled::show(MDB_OLED_ADDR);
}

static void show_error(const char* msg) {
  Serial.printf("[selector] ERROR: %s\n", msg);
  if (!oled_ok) return;
  mdb_oled::clear();
  mdb_oled::text(0, 0, "ERROR", 1, true);
  // Wrap msg into ~21-char lines (128 px / 6 px advance). Most ESP error
  // names like ESP_ERR_OTA_VALIDATE_FAILED need two rows.
  constexpr int CHARS_PER_LINE = mdb_oled::W / 6;
  constexpr int MAX_LINES = 4;
  int len = (int)strlen(msg);
  for (int line = 0; line < MAX_LINES; line++) {
    int off = line * CHARS_PER_LINE;
    if (off >= len) break;
    char buf[CHARS_PER_LINE + 1];
    int n = len - off;
    if (n > CHARS_PER_LINE) n = CHARS_PER_LINE;
    memcpy(buf, msg + off, n);
    buf[n] = '\0';
    mdb_oled::text(0, 12 + line * 10, buf, 1, true);
  }
  mdb_oled::show(MDB_OLED_ADDR);
}

// Reset the boot-in-progress flags after a failed boot attempt so the LED
// task resumes its normal selection-blink and a subsequent button press
// re-renders the menu over the error. Set the pending-release flag so a
// still-held long-press doesn't immediately re-fire the same failed boot.
static void cancel_boot_attempt() {
  g_booting = false;
  auto_boot_cancelled = true;
  g_boot_failed_pending_release = true;
}

static void boot_into(Slot s) {
  g_booting = true;
  mdb_pref::save((uint8_t)s);
  const esp_partition_t* p = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, slot_subtype(s), nullptr);
  if (!p) {
    show_error("slot not found");
    delay(3000);
    cancel_boot_attempt();
    return;
  }
  esp_err_t err = esp_ota_set_boot_partition(p);
  if (err != ESP_OK) {
    show_error(esp_err_to_name(err));
    delay(3000);
    cancel_boot_attempt();
    return;
  }
  Serial.printf("[selector] booting %s @ 0x%06lx\n",
                slot_name(s), (unsigned long)p->address);
  draw_loading(s);
  delay(400);
  esp_restart();
}

static Slot load_selection() {
  uint8_t v = mdb_pref::load((uint8_t)SLOT_0);
  return v < N_SLOTS ? (Slot)v : SLOT_0;
}

void setup() {
  Serial.begin(115200);
  // Boot pause: USB-CDC takes ~1 s to enumerate after a reset; this lets
  // a host-side serial monitor connect and not miss the early diagnostics.
  delay(2000);
  Serial.println(F("\n[selector] mesh-dualboot starting"));

  pinMode(MDB_PIN_BUTTON, INPUT_PULLUP);

  // ESP32-S3 RTC GPIO may be holding our pins from a prior reset
  // (e.g. if the previously-booted firmware called rtc_gpio_hold_en).
  // Release any RTC hold/routing before driving these pins as normal GPIOs.
#ifdef MDB_PIN_VEXT_EN
  rtc_gpio_hold_dis((gpio_num_t)MDB_PIN_VEXT_EN);
  rtc_gpio_deinit((gpio_num_t)MDB_PIN_VEXT_EN);
#endif
#ifdef MDB_PIN_LORA_PA_POWER
  rtc_gpio_hold_dis((gpio_num_t)MDB_PIN_LORA_PA_POWER);
  rtc_gpio_deinit((gpio_num_t)MDB_PIN_LORA_PA_POWER);
#endif

  pinMode(MDB_PIN_LED, OUTPUT);
  digitalWrite(MDB_PIN_LED, LOW);

  // ── Power-on sequencing ────────────────────────────────────────────────
  // Heltec V4/V4.3 OLED rail (Vext) is gated by a P-channel MOSFET driven
  // by GPIO 36. Vext is *active LOW* (the official Heltec library defines
  // VEXT_ON_LEVEL=LOW; confirmed by https://wiki.heltec.org/ V4 schematic).
  // We do an explicit RESET pulse for the SSD1315 ourselves before init().
  //
  // We DELIBERATELY do not call pinMode(SDA,INPUT)/pinMode(SCL,INPUT) for
  // a "bus health" sanity read after Wire.begin(): doing so detaches SDA
  // and SCL from the I2C peripheral via the GPIO matrix, and the second
  // Wire.begin() that would re-attach them is a no-op (the Arduino-ESP32
  // TwoWire::begin returns true without re-running initPins when the bus
  // is already initialized — see Wire.cpp:300-303). The result is err=5
  // timeouts on every transaction even though the bus pull-ups look fine.
  // ───────────────────────────────────────────────────────────────────────

#ifdef MDB_PIN_VEXT_EN
  pinMode(MDB_PIN_VEXT_EN, OUTPUT);
  digitalWrite(MDB_PIN_VEXT_EN, MDB_PIN_VEXT_ACTIVE);
#endif

#if (MDB_PIN_OLED_RESET) >= 0
  // Hold OLED in reset while Vext settles, then release.
  pinMode(MDB_PIN_OLED_RESET, OUTPUT);
  digitalWrite(MDB_PIN_OLED_RESET, LOW);
#endif

#if defined(MDB_HAS_AXP2101) && MDB_HAS_AXP2101
  // T-Beam Supreme: AXP2101 PMU on Wire1 gates the OLED via DCDC1.
  if (!init_pmu()) {
    Serial.println(F("[selector] AXP2101 init failed"));
  } else {
    Serial.println(F("[selector] AXP2101 DCDC1 enabled at 3.3V"));
  }
#endif

#ifdef MDB_PIN_LORA_PA_POWER
  // GPIO 7 powers the LoRa FEM LDO on V4.3. Not on the OLED rail, but
  // MeshCore drives this HIGH at boot; doing the same avoids any chance
  // of leakage paths via the SX1262 module that could affect Vext.
  pinMode(MDB_PIN_LORA_PA_POWER, OUTPUT);
  digitalWrite(MDB_PIN_LORA_PA_POWER, HIGH);
#endif

  delay(50);  // Vext rise + FEM LDO startup

#if (MDB_PIN_OLED_RESET) >= 0
  digitalWrite(MDB_PIN_OLED_RESET, HIGH);
  delay(10);  // SSD1315 t_RESET datasheet min 3 us; be generous
#endif

  g_phase = 1;
  led_start_indicator();

  Wire.begin(MDB_PIN_SDA, MDB_PIN_SCL);
  Wire.setClock(100000);
  g_phase = 2;

  Serial.println(F("[selector] I2C scan 0x08..0x77:"));
  int found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("[selector]   ACK at 0x%02x\n", addr);
      found++;
    } else if (err != 2) {
      Serial.printf("[selector]   err %u at 0x%02x\n", err, addr);
    }
  }
  Serial.printf("[selector] scan done: %d device(s) found\n", found);
  Serial.flush();
  g_i2c_count = found;
  g_phase = 3;

  oled_ok = init_oled();
  g_phase = 4;

  if (oled_ok) {
    Serial.println(F("[selector] OLED OK"));
  } else {
    Serial.printf("[selector] OLED unavailable (i2c_count=%d) — LED-only mode\n", g_i2c_count);
  }
  // In LED-only mode, never auto-boot — wait for explicit PRG long-press.
  if (!oled_ok) auto_boot_cancelled = true;

  current = load_selection();
  g_selection_for_led = (int)current;
  Serial.printf("[selector] loaded last selection: %s\n", slot_name(current));
  if (digitalRead(MDB_PIN_BUTTON) == LOW) auto_boot_cancelled = true;

  if (oled_ok) {
    draw_splash();
    delay(700);
    auto_boot_start_ms = millis();
    draw_menu(MDB_AUTO_BOOT_MS);
  }
  g_phase = 5;
}

void loop() {
  static int last_btn = HIGH;
  static unsigned long btn_down_ms = 0;
  static long last_drawn_ms = -1;

  unsigned long now = millis();
  int btn = digitalRead(MDB_PIN_BUTTON);

  if (btn != last_btn) {
    delay(DEBOUNCE_MS);
    btn = digitalRead(MDB_PIN_BUTTON);
    if (btn != last_btn) {
      last_btn = btn;
      if (btn == LOW) {
        btn_down_ms = now;
        auto_boot_cancelled = true;
        g_user_input_seen = true;
        last_drawn_ms = -1;
        draw_menu(0, 1);  // start hold-progress at minimum so it's visible
      } else {
        // Release before threshold = short tap = cycle.
        // (Long-press fires immediately on threshold below; release path
        // for that case won't reach here because boot_into restarts.)
        unsigned long held = now - btn_down_ms;
        if (held < LONG_PRESS_MS && !g_boot_failed_pending_release) {
          current = (Slot)(((int)current + 1) % N_SLOTS);
          g_selection_for_led = (int)current;
          Serial.printf("[selector] cycled to %s\n", slot_name(current));
          draw_menu(0);
        }
        // Any release clears the pending-release latch from a prior failed boot.
        g_boot_failed_pending_release = false;
      }
    }
  }

  // While button is held: redraw hold progress, fire boot when threshold crossed.
  // If a prior boot attempt failed and the user is still holding from then,
  // require them to release first before another long-press can re-fire.
  if (last_btn == LOW && !g_boot_failed_pending_release) {
    long held = (long)(now - btn_down_ms);
    if (held >= (long)LONG_PRESS_MS) {
      boot_into(current);
      return;
    }
    // Animate the progress bar while held (~10 Hz).
    if (last_drawn_ms < 0 || (held - last_drawn_ms) >= 50) {
      last_drawn_ms = held;
      draw_menu(0, (int)held);
    }
  }

  if (!auto_boot_cancelled) {
    long elapsed = (long)(now - auto_boot_start_ms);
    long remaining = (long)MDB_AUTO_BOOT_MS - elapsed;
    if (remaining < 0) remaining = 0;
    if (last_drawn_ms < 0 || (last_drawn_ms - remaining) >= 100) {
      last_drawn_ms = remaining;
      draw_menu((int)remaining);
    }
    if (elapsed >= (long)MDB_AUTO_BOOT_MS) {
      boot_into(current);
    }
  }

  // Heartbeat — every 2s, print state plus live probe.
  static unsigned long last_hb_ms = 0;
  if (now - last_hb_ms >= 2000) {
    last_hb_ms = now;
    Wire.beginTransmission(MDB_OLED_ADDR);
    uint8_t live_err = Wire.endTransmission();
    Serial.printf("[selector] alive sel=%s oled=%d phase=%d cancel=%d  live_probe(0x%02x)=%u\n",
                  slot_name(current), (int)oled_ok, g_phase,
                  (int)auto_boot_cancelled, MDB_OLED_ADDR, live_err);
    Serial.flush();
    // If the OLED has come up since boot, attempt to init it now.
    if (!oled_ok && live_err == 0) {
      Serial.println(F("[selector] OLED now ACKing — initializing display"));
      oled_ok = init_oled();
      if (oled_ok) {
        Serial.println(F("[selector] OLED init succeeded — drawing menu"));
        draw_splash();
        delay(700);
        // Only resume the auto-boot countdown if the user hasn't already
        // expressed a preference via PRG. Otherwise their explicit
        // cancellation in LED-only mode would be silently undone.
        if (!g_user_input_seen) {
          auto_boot_start_ms = millis();
          auto_boot_cancelled = false;
          draw_menu(MDB_AUTO_BOOT_MS);
        } else {
          draw_menu(0);
        }
      }
    }
  }

  delay(20);
}
