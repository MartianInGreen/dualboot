# Porting mesh-dualboot to other devices

The selector is intentionally a tiny piece of code. Almost everything
that varies between boards is supplied via `-D` macros in the selector's
`platformio.ini` — board changes do not require modifying any source.

## Minimum requirements

A board is a candidate for mesh-dualboot if it meets all of:

1. **ESP32 family** with at least **8 MB of flash**. The chip must
   support OTA partitions and `esp_ota_set_boot_partition()` (any
   ESP32, ESP32-S2, ESP32-S3, ESP32-C3 or newer).
2. **Two compatible firmwares** that you want to switch between. Each
   needs:
   - A PlatformIO build env that targets the dualboot partition CSV.
   - A cooperative-return hook (PRG-held at boot → re-target factory
     and `esp_restart`). See `MeshDualbootHook.h`.
   - Each firmware ≤ the OTA slot size (3.1875 MB on 8 MB layouts,
     5.25 MB on 16 MB layouts). Resize the partition table if needed.
3. **A user button** (any GPIO with internal pull-up) for cycling and
   confirmation.
4. **Optional but recommended**: an I²C OLED for the menu, and a
   built-in LED for blink-code diagnostics. Boards without a display
   still work — the selector falls back to LED-blink mode where
   blink count = current selection (1 = ota_0, 2 = ota_1) and a
   long-press boots.

## Selector configuration macros

Set these in a new `[env:<board>]` block in `selector/platformio.ini`.

| Macro | Required | Purpose |
|---|---|---|
| `MDB_DISPLAY_KIND` | yes | `0` = SSD1306/SSD1315, `1` = SH1106. Selects which init sequence and column offset the bundled `mdb_oled` driver applies. |
| `MDB_PIN_SDA` | yes | I²C SDA for the OLED |
| `MDB_PIN_SCL` | yes | I²C SCL for the OLED |
| `MDB_OLED_ADDR` | yes | I²C address (almost always `0x3C`) |
| `MDB_PIN_OLED_RESET` | yes | Reset pin, or `-1` if not wired |
| `MDB_PIN_BUTTON` | yes | User button, active-LOW (uses `INPUT_PULLUP`) |
| `MDB_PIN_LED` | optional | Status LED. Skip if there isn't one. |
| `MDB_AUTO_BOOT_MS` | yes | Auto-boot countdown in ms (e.g. `5000`) |
| `MDB_PIN_VEXT_EN` | optional | OLED-rail power-gate pin (Heltec-style) |
| `MDB_PIN_VEXT_ACTIVE` | with VEXT_EN | `LOW` or `HIGH` — verify against board datasheet, **not** the variant config of an existing firmware (those are sometimes wrong; see `selector/src/main.cpp` comments) |
| `MDB_PIN_LORA_PA_POWER` | optional | LoRa FEM LDO enable; HIGH at boot to prevent leakage paths through the radio. Heltec V4 uses pin 7 |
| `MDB_HAS_AXP2101` | optional | `1` if an AXP2101 PMU gates the OLED via DCDC1 (T-Beam Supreme) |
| `MDB_PIN_PMU_SDA` | with AXP2101 | I²C SDA for the PMU bus (often a separate Wire1) |
| `MDB_PIN_PMU_SCL` | with AXP2101 | I²C SCL for the PMU bus |

## Step-by-step: adding a new board

1. **Pick or design a partition table.** If the board has 8 MB use
   `partitions_8MB.csv` + `partitions_app_8MB.csv`. If 16 MB use
   `partitions.csv` + `partitions_app.csv`. For other flash sizes,
   design a CSV with: nvs (16 KB), nvs_sel (12 KB), otadata (8 KB),
   factory (≥ 320 KB; selector currently fits in ~290 KB), ota_0,
   ota_1, two filesystem partitions (one per firmware), and a
   coredump partition. Verify offsets: app partitions must be 64 KB
   aligned; data partitions 4 KB aligned.

2. **Add a board JSON** under `boards/` describing the chip, flash
   size, and `maximum_size` (set to the **factory** slot size — the
   selector's slot — so PlatformIO's checkprogsize gates the build).
   Set `variant: "esp32s3"` (or appropriate stock variant) unless you
   need a custom one.

3. **Add a new env** in `selector/platformio.ini`:
   - `board = <your board>` (matching the JSON file name)
   - `board_build.partitions = ../partitions_<size>.csv`
   - All applicable `MDB_*` macros from the table above
   - `lib_deps`: `XPowersLib` if `MDB_HAS_AXP2101=1` (the OLED driver
     is in-tree at `selector/src/mdb_oled.*` and needs no external
     library; `Wire` is in the framework)
   - For ESP32-S3 boards specifically, prefer HWCDC over TinyUSB CDC
     for Serial diagnostics — TinyUSB drags in ~30 KB of unused
     device classes. If your board JSON sets `ARDUINO_USB_MODE=0`,
     override with `build_unflags = -DARDUINO_USB_MODE=0` and
     `build_flags = -DARDUINO_USB_MODE=1`. (See `[env:heltec_v4]`
     for a worked example.)

4. **Build and verify** the selector size:
   `pio run -e <your-env>` — should report a Flash usage well under
   the factory slot. Common issue: chosen variant's stock pin
   defaults conflict with framework startup (rare).

5. **Add dualboot envs to your two firmware projects** that:
   - Point `board_build.partitions` at the matching `partitions_app_*.csv`
   - Add `-D MDB_DUALBOOT=1`
   - Apply minimal source patches: include `MeshDualbootHook.h` and
     call `mdb::check_return_to_selector()` at the top of `setup()`.
   - For MeshCore companion-radio specifically, also use a separate
     SPIFFS label so the two firmwares don't trample each other's
     filesystem partitions:
     `-D MC_SPIFFS_LABEL=\"mc_spiffs\"` and
     `SPIFFS.begin(true, "/spiffs", 10, MC_SPIFFS_LABEL)`.

6. **Add the board to `scripts/flash_all.py`** by extending the
   `BOARDS` dict with `flash_size`, the selector env name, partition
   offsets, and slot sizes.

7. **Add the board to `slots.json`** with two entries (`name` + `build`)
   defining what goes in slot 0 and slot 1. The `name` is what the
   selector's OLED shows and what `--only <name>` accepts on the
   flasher CLI; the `build` block tells the flasher how to find the
   default firmware bin (`{"kind":"meshcore","env":"..."}`,
   `{"kind":"meshtastic","env":"..."}`, or
   `{"kind":"literal","path":"..."}`).

8. **Smoke-test on hardware**: full-chip flash, power-cycle, see the
   menu, cycle, long-press, observe each firmware boot, confirm the
   cooperative return-to-selector hook works (PRG held at firmware
   startup).

## A note on A/B OTA (it's incompatible with dual-boot)

Standard ESP-IDF / Arduino-ESP32 over-the-air updates assume `ota_0`
and `ota_1` are two slots of the *same* firmware: the running firmware
writes the new image to whichever slot is *not* currently active and
flips `otadata` to point at it on the next boot. We've claimed both
slots for two *different* firmwares, so an in-firmware OTA writes the
new image into the *other* firmware's slot, overwriting it and leaving
`otadata` pointing at the wrong slot. The selector then offers menu
entries that no longer correspond to what's actually installed — e.g.
choosing "MeshCore" would launch the freshly OTA'd Meshtastic that
just landed where MeshCore used to live.

**Disable in-firmware OTA on both sides** (or simply do not use the
web-flasher / web OTA endpoints) when running mesh-dualboot. Update by
USB with `flash_all.py --only <slot>` instead.

If you really need A/B OTA, give up the dual-boot and let one firmware
own both slots itself.

## A note on the OLED-power-polarity gotcha

Heltec V4's OLED rail (`VEXT_EN`) is **active LOW** despite at least
one major firmware having `ACTIVE=HIGH` in its variant config. Always
validate polarity against the board's official schematic / vendor
library, not against another firmware's variant defines, which can
quietly disagree with hardware and survive only because the variant
never explicitly drives the gate.

If the OLED's I²C bus reads `SDA=1 SCL=1` at idle but every transaction
times out (err 5), the rail is unpowered (no pull-ups online). Flip
the polarity.

If the bus reads `SDA=0 SCL=0` at idle, you're driving the gate to the
inactive state — same fix, opposite direction.

## A note on the `pinMode(SDA, INPUT)` footgun

After `Wire.begin(SDA, SCL)`, calling `pinMode(SDA, INPUT)` (e.g. for
a "bus health" sanity read) detaches SDA from the I²C peripheral via
the GPIO matrix. The natural recovery — calling `Wire.begin()` again —
is a no-op on Arduino-ESP32 (`TwoWire::begin` early-returns when
`i2cIsInit` is already true). Pins stay detached, every transaction
returns err 5, but the bus *looks* fine because external pull-ups
still hold the lines high. If you must read raw bus levels, do it
**before** `Wire.begin()`, or use `Wire.end()` first.
