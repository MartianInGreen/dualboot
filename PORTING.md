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
| `MDB_PIN_LED` | optional | Status LED, driven **active HIGH** (HIGH = lit). There is no polarity macro — an active-LOW LED blinks inverted, which is cosmetic only. Skip if there isn't one. |
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

## Worked example: Seeed XIAO ESP32S3 + Wio-SX1262 (8 MB, no display)

This is the minimum-effort port: an 8 MB ESP32-S3 that reuses
`partitions_8MB.csv` verbatim, and the first target with **no display**,
which exercises the LED-only fallback path. Total selector-side change:
one board JSON, two `[env:...]` blocks (plain + net-OTA), one
`slots.json` entry, one `BOARDS` entry in `flash_all.py` — no source
changes at all.

### Hardware facts that drive the config

| Fact | Value | Consequence |
|---|---|---|
| Flash | 8 MB, QIO, 80 MHz | reuse `partitions_8MB.csv` (3.1875 MB per OTA slot; the net-OTA layout trims to 3.125 MB) |
| PSRAM | 8 MB octal | board JSON needs `memory_type: qio_opi` |
| User button | Wio-SX1262 module button, **GPIO21**, active LOW, external 10K pull-up | `MDB_PIN_BUTTON=21`; not the GPIO0 strap pin, so holding it at power-on is safe |
| Status LED | green LED, **GPIO48**, active HIGH | `MDB_PIN_LED=48` (matches the driver's HIGH = lit convention) |
| Display | none on the bare kit; XIAO Expansion Board SSD1306 at `0x3C` on **GPIO5 (SDA) / GPIO6 (SCL)** | probe-and-fallback; `MDB_PIN_OLED_RESET=-1` |
| LoRa | SX1262, SPI1 SCK 7 / MISO 8 / MOSI 9 / CS 41 / RST 42 / BUSY 40 / DIO1 39 / RXEN 38, TX/RX via DIO2 | selector ignores it — no FEM LDO to enable, no `MDB_PIN_LORA_PA_POWER` |

Pin values are taken from the two upstream firmware variants, which are
the de-facto schematic for this kit: MeshCore `variants/xiao_s3_wio`
(`PIN_USER_BTN=21`, `PIN_STATUS_LED=48`, `PIN_BOARD_SDA=D4`,
`PIN_BOARD_SCL=D5`) and Meshtastic `variants/esp32s3/seeed_xiao_s3`
(`BUTTON_PIN 21`, `LED_POWER 48`, `LED_STATE_ON 1`, `I2C_SDA 5`,
`I2C_SCL 6`). Re-check both if Seeed revs the kit.

### Selector side

`boards/xiao_s3_wio.json` mirrors PlatformIO's `seeed_xiao_esp32s3`
manifest (variant `XIAO_ESP32S3`, `memory_type: qio_opi`, HWCDC on boot,
`flash_size: 8MB`) with `maximum_size` set to the **factory** slot size
(`0x60000`) so `checkprogsize` gates the selector build against the right
ceiling.

`selector/platformio.ini` gets one plain env plus its net-OTA sibling:

```ini
[env:xiao_s3_wio]
platform = espressif32@6.11.0
board = xiao_s3_wio
board_build.partitions = ../partitions_8MB.csv
build_flags =
  -D MDB_BOARD_XIAO_S3_WIO=1
  -D MDB_DISPLAY_KIND=0
  -D MDB_PIN_SDA=5
  -D MDB_PIN_SCL=6
  -D MDB_PIN_OLED_RESET=-1
  -D MDB_PIN_BUTTON=21
  -D MDB_PIN_LED=48
  -D MDB_OLED_ADDR=0x3C
  -D MDB_AUTO_BOOT_MS=5000
build_src_filter = +<*> -<net_ota/>

[env:xiao_s3_wio_netota]
extends = env:xiao_s3_wio
board_build.partitions = ../partitions_8MB_netota.csv
build_flags =
  ${env:xiao_s3_wio.build_flags}
  -D MDB_NET_OTA=1
  -D MDB_NET_OTA_AP=1
  -D MDB_NET_OTA_STA=1
  -D MDB_NET_OTA_HEADLESS=1
  -D MDB_NET_OTA_AUTH_HMAC=1
build_src_filter = +<*> +<net_ota/>
```

Note there is **no separate "with display" env**. `main.cpp` already
probes the OLED address and falls back to LED mode on failure, so one
binary covers both the bare kit and the expansion-board configuration.
Keeping one env also means the `slots.json` key (`xiao_s3_wio`) and the
`--board` key stay single-valued.

One gotcha this port exposed: `slots_build.py` keys the pre-script's
`slots.json` lookup on the PlatformIO env name, so `xiao_s3_wio_netota`
missed the entry and silently fell back to the compiled-in default labels
(`MeshCore`/`Meshtastic`) instead of `Official`/`MeshCore`. The pre-script
now strips a trailing `_netota` before lookup, which fixes the same bug
for the Heltec and T-Beam net-OTA builds.

### Firmware side

MeshCore — `variants/xiao_s3_wio/platformio.ini`, next to the existing
`Xiao_S3_WIO_companion_radio_ble` env:

```ini
[env:Xiao_S3_WIO_companion_radio_ble_dualboot]
extends = env:Xiao_S3_WIO_companion_radio_ble
board_build.partitions = ../../dualboot/partitions_app_8MB.csv
build_flags =
  ${env:Xiao_S3_WIO_companion_radio_ble.build_flags}
  -D MDB_DUALBOOT=1
  -D MDB_BUTTON_PIN=21
  -D MC_SPIFFS_LABEL=\"mc_spiffs\"
  -D BLE_NAME_PREFIX=\"MC_\"
```

Meshtastic — `variants/esp32s3/seeed_xiao_s3/platformio.ini`, next to
`seeed-xiao-s3`:

```ini
[env:seeed-xiao-s3-dualboot]
extends = env:seeed-xiao-s3
board_build.partitions = ../../dualboot/partitions_app_8MB.csv
build_flags =
  ${env:seeed-xiao-s3.build_flags}
  -D MDB_DUALBOOT=1
  -D MDB_BUTTON_PIN=21
```

Both envs also need the two source patches from the T-Beam port (copy
`MeshDualbootHook.h` in, and `#include` +
`mdb::check_return_to_selector()` at the top of `setup()`).

### The one non-obvious trap: `MDB_BUTTON_PIN`

`MeshDualbootHook.h` defaults to:

```c
#ifndef MDB_BUTTON_PIN
#define MDB_BUTTON_PIN 0
#endif
```

GPIO0 is right for the Heltec (PRG) and the T-Beam, and *wrong* for this
kit — the user button is GPIO21. Forget the define and hold-to-return
silently watches the BOOT button, which is also the ROM download strap:
holding that one through a power cycle re-enumerates the chip as a serial
loader instead of returning to the selector, so it looks like a brick to
anyone who does not know the strap exists.

Related: because the selector's button (21) is *not* a strap pin, the
XIAO does not need the "don't hold it at power-on" caveat that the Heltec
has. Hold-through-boot works.

### LED-only mode semantics

With no OLED the selector sets `auto_boot_cancelled = true` (see
`setup()`), so it waits for an explicit long-press instead of counting
down into a firmware. The LED task blinks the *current* selection — 1
blink for slot 0, 2 for slot 1 — on a ~2 s cycle, and taps immediately
update the blink count. `MDB_AUTO_BOOT_MS` is consequently inert on a
headless board; it only matters once a panel is attached.

Keep the `MDB_AUTO_BOOT_MS` define anyway: the same binary is what runs
when an expansion board is fitted, and the auto-boot path needs it.

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
USB with `flash_all.py --only <slot>` instead, or — for phone-only or
remote/headless scenarios — use the **selector-mediated net-OTA path**
described below, which writes directly to ota_X via `esp_ota_write`
without ever touching `otadata`.

If you really need A/B OTA, give up the dual-boot and let one firmware
own both slots itself.

## Net-OTA (compile-time optional)

The selector can be built with a WiFi+TCP listener that authenticates an
incoming firmware bin and streams it into a chosen ota slot. This solves
the "I'm in the field with only a phone" and "I want to update headless
fleet devices remotely" scenarios — neither covered by USB-only flashing
or in-firmware OTA.

### Partition coupling

Net-OTA is enabled by switching to `partitions_netota.csv`, which grows
the `factory` partition from 384 KB to 1 MB so the selector can link in
WiFi + lwIP + auth + UI. **Both firmware projects must use the matching
`partitions_app_netota.csv`** when the device is built alongside a
net-OTA-enabled selector — the offsets shift, and a mismatch between
the selector-side table and the firmware-side app-view will boot the
wrong bytes.

In each firmware's dualboot env:
```ini
board_build.partitions = ../../path/to/dualboot/partitions_app_netota.csv
```

### Build flag matrix (selector-side)

| Flag | Purpose |
|---|---|
| `MDB_NET_OTA=1` | Master enable — gates partition table choice and `net_ota/` source-filter inclusion |
| `MDB_NET_OTA_AP=1` | Include softAP transport (selector hosts AP) |
| `MDB_NET_OTA_STA=1` | Include station transport (selector joins WiFi from NVS-stored creds) |
| `MDB_NET_OTA_HEADLESS=1` | Include NVS-flag trigger; pairs with the firmware-side helpers below |
| `MDB_NET_OTA_AUTH_HMAC=1` | Accept HMAC-SHA256-authenticated bins |
| `MDB_NET_OTA_AUTH_ED25519=1` | Accept Ed25519-signed bins (requires `MDB_NET_OTA_PUBKEY_HEX="..."`) |

`#error` invariants in `mdb_net_ota.h` enforce: at least one transport,
at least one auth mode, and `_PUBKEY_HEX` is set whenever `_AUTH_ED25519` is.

### Ed25519 setup

Ed25519 verify uses [orlp/ed25519](https://github.com/orlp/ed25519)
(Zlib license, single-folder C, RFC 8032 standard). It's wired in as a
git submodule under `selector/lib/ed25519` rather than vendored, so the
upstream is git-pinned and visible in `git submodule status`. PlatformIO
auto-discovers `lib/ed25519/src/*.c` and links it in only when
`MDB_NET_OTA_AUTH_ED25519` is defined (LTO drops it otherwise).

After cloning this repo:
```bash
git submodule update --init --recursive
```

To enable signed bins on a build:
1. Generate a keypair offline (e.g., `openssl genpkey -algorithm Ed25519 -out priv.pem` plus a small Python tool to extract the 32-byte raw pubkey).
2. In `selector/platformio.ini`'s `[env:heltec_v4_netota]`, uncomment:
   ```ini
   -D MDB_NET_OTA_AUTH_ED25519=1
   -D MDB_NET_OTA_PUBKEY_HEX="\"<64-char hex>\""
   ```
3. Rebuild. Selector now accepts wire bins with `auth_type=0x02` whose
   64-byte signature verifies against your pubkey over `sha256(prefix||payload)`.

The signing tool computes `sha256(prefix || payload)` and signs the 32-byte
hash with Ed25519 (sign-the-hash, not sign-the-bin — matches the selector's
streaming-friendly design where the SHA-256 is accumulated during write).

Cost: ~25 KB binary growth (sha512 + ge/fe/sc curve operations + verify
logic). Default 1 MB factory has plenty of headroom.

**Don't bake an all-zeros placeholder pubkey.** It pairs with a known
weak private key; anyone could sign bins for your devices.

### Firmware-side helpers

`MeshDualbootHook.h` (in both forks) exposes two helpers when the
firmware build defines `MDB_NET_OTA_HEADLESS` and/or `MDB_NET_OTA_STA`:

```c
namespace mdb {
  // Persist station-mode WiFi creds in nvs_sel without triggering a session.
  bool set_net_flash_wifi(const char* ssid, const char* psk);

  // Stash creds + reboot. Selector picks up nf_pending and listens for
  // `timeout_s` seconds. The CLIENT picks the target slot in its wire
  // header (hybrid design — no caller-bound slot in headless mode).
  bool request_net_flash(const char* ssid, const char* psk, uint16_t timeout_s);
}
```

Wire these into the firmware's existing command channel (Meshtastic
admin commands, MeshCore's client API, etc.) so an operator can issue
them remotely. That wiring is firmware-specific and out of scope for
the dualboot repo itself.

### Why this is safe for dualboot (unlike in-firmware OTA)

The net-OTA writer runs from the `factory` partition (the selector),
not from an OTA slot. Both ota_0 and ota_1 are quiescent, and the
writer addresses the target by partition subtype — same logical
operation as `flash_all.py --only`. **Otadata is never touched**, so
the selector keeps boot ownership; the user re-picks via menu after
the write. None of the failure modes that make in-firmware OTA unsafe
on dualboot apply here.

### 8 MB layout (T-Beam Supreme, XIAO ESP32S3 + Wio-SX1262)

Supported via `partitions_8MB_netota.csv` and `partitions_app_8MB_netota.csv`,
shared by both 8 MB targets (`xiao_s3_wio_netota` simply extends
`xiao_s3_wio` with the net-OTA flags).
Tighter than the 16 MB version; trade-offs are baked into the CSV:
- factory:    384 KB → 768 KB  (+384 KB)
- ota_X:    3.1875 MB → 3.125 MB (-64 KB each)
- spiffs:     512 KB → 384 KB   (-128 KB each)

Watch for: if Meshtastic ever grows past 3.125 MB, ota slots need to
move. Re-test after each Meshtastic release. The selector itself is at
~95% of the 768 KB factory in HMAC-only mode; enabling Ed25519 brings
it to ~99% — workable, but no headroom for further growth. The XIAO
build links without XPowersLib (no PMU on this board), so it lands a few
KB smaller than the T-Beam's — do not treat that as headroom.

Firmware-side envs targeting an 8 MB net-OTA device must use
`partitions_app_8MB_netota.csv` so checkprogsize gates against the
3.125 MB ceiling. Building against the older `partitions_app_8MB.csv`
(3.1875 MB ceiling) and flashing into the netota layout works as long
as the firmware bin is under 3.125 MB, but won't catch overgrowth.

### What's NOT covered

- **Selector self-update**: factory is never written by net-OTA. Only
  USB reflash can replace the selector. Rationale: a corrupted
  self-update bricks the device with no recovery path.

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
