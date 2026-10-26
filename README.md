# mesh-dualboot

Run **MeshCore** or **Meshtastic** on the same ESP32-S3 device, picked at boot
from a menu — no reflashing.

Supported targets:
- **Heltec WiFi LoRa 32 v4** (16 MB flash, SSD1315 OLED) — hardware-verified.
- **LilyGo T-Beam Supreme** (8 MB flash) — hardware-verified.
- **Seeed XIAO ESP32S3 + Wio-SX1262 kit** (8 MB flash, no built-in display) —
  builds and partition-verified; LED-blink menu out of the box, full OLED
  menu if a XIAO Expansion Board is clipped on (see
  [XIAO notes](#seeed-xiao-esp32s3--wio-sx1262-notes)).

**Project home:** [officialmesh.org](https://officialmesh.org) · [github.com/official-mesh](https://github.com/official-mesh)  
**Maintainer:** The Official Mesh Admin  
**License:** GNU Affero General Public License v3.0-only — see [`LICENSE`](LICENSE)

> Coexistence tool for [Meshtastic](https://meshtastic.org/) and [MeshCore](https://github.com/ripplebiz/MeshCore) firmware images. Both project names are referenced nominatively. This project is **not affiliated with, endorsed by, licensed from, or authorized by Meshtastic LLC, the MeshCore project, or any of their maintainers**.

## Related projects

This tool ships alongside five companion repos under the `official-mesh` org:

| Repo | Purpose |
|------|---------|
| [`official-mesh/firmware`](https://github.com/official-mesh/firmware)   | Meshtastic firmware fork — adds privacy modules |
| [`official-mesh/android`](https://github.com/official-mesh/android)     | Android client for the firmware fork |
| [`official-mesh/python`](https://github.com/official-mesh/python)       | Python CLI for the firmware fork |
| [`official-mesh/meshbot`](https://github.com/official-mesh/meshbot)     | LLM-powered mesh radio bot; hosts the org's CLA and Code of Conduct |
| [`official-mesh/remote`](https://github.com/official-mesh/remote)       | Android remote-control surface for meshbot |

## How it works

A small **selector** app lives in the `factory` partition. On power-on, the
ESP32 bootloader sees an empty `otadata` and boots `factory`. The selector
draws a two-item menu, lets you cycle with PRG, and on selection calls
`esp_ota_set_boot_partition()` + `esp_restart()` to hand control to either
slot. Slot assignments are configured in [`slots.json`](slots.json); by
default:

* `ota_0` → MeshCore
* `ota_1` → Meshtastic

Each firmware then auto-boots on subsequent power cycles. To return to the
selector you have two options at any time:

* Hold **PRG** for ~2 seconds during the firmware's startup window, or
* **Double-tap RST** within ~3 seconds.

Both routes are implemented in `MeshDualbootHook.h` (a tiny header included
into each firmware's `setup()`), backed by RTC slow memory so the
double-reset counter survives a reset but not a power-off.

On boards without a display (the bare XIAO kit) the selector falls back to
an **LED-blink menu**: it blinks the current selection (1 = slot 0,
2 = slot 1), a tap cycles, and a hold ≥0.5 s boots. No auto-boot countdown,
so a headless node never picks a firmware without someone pressing the
button.

## Layout

`partitions.csv` (16 MB — Heltec V4):

| Offset    | Size     | Name      | Purpose                          |
|-----------|----------|-----------|----------------------------------|
| 0x009000  | 16 KB    | nvs       | shared NVS (Arduino Preferences) |
| 0x00d000  | 12 KB    | nvs_sel   | selector's own NVS               |
| 0x010000  | 8 KB     | otadata   | which app to boot                |
| 0x020000  | 384 KB   | factory   | selector app                     |
| 0x080000  | 5.25 MB  | ota_0     | **MeshCore**                     |
| 0x5c0000  | 5.25 MB  | ota_1     | **Meshtastic**                   |
| 0xb00000  | 2 MB     | spiffs    | Meshtastic LittleFS              |
| 0xd00000  | 2 MB     | mc_spiffs | MeshCore SPIFFS                  |
| 0xf00000  | 64 KB    | coredump  | crash dumps                      |

`partitions_8MB.csv` (8 MB — T-Beam Supreme, XIAO ESP32S3 + Wio-SX1262):

| Offset    | Size      | Name      | Purpose                          |
|-----------|-----------|-----------|----------------------------------|
| 0x009000  | 16 KB     | nvs       | shared NVS                       |
| 0x00d000  | 12 KB     | nvs_sel   | selector's own NVS               |
| 0x010000  | 8 KB      | otadata   | which app to boot                |
| 0x020000  | 384 KB    | factory   | selector app                     |
| 0x080000  | 3.125 MB  | ota_0     | **MeshCore**                     |
| 0x3b0000  | 3.125 MB  | ota_1     | **Meshtastic**                   |
| 0x6e0000  | 512 KB    | spiffs    | Meshtastic LittleFS              |
| 0x760000  | 512 KB    | mc_spiffs | MeshCore SPIFFS                  |
| 0x7e0000  | 64 KB     | coredump  | crash dumps                      |

Each firmware has its own filesystem partition so they don't trample each
other's persistent state.

## Build & flash

Three projects, three builds. Substitute your board:

- Heltec V4: selector env `heltec_v4`, MeshCore env `heltec_v4_companion_radio_ble_dualboot`, Meshtastic env `heltec-v4-dualboot`.
- T-Beam Supreme (8 MB): selector env `t_beam_supreme`, MeshCore env `T_Beam_S3_Supreme_SX1262_companion_radio_ble_dualboot`, Meshtastic env `tbeam-s3-core-dualboot`.
- XIAO ESP32S3 + Wio-SX1262 (8 MB): selector env `xiao_s3_wio`, MeshCore env `Xiao_S3_WIO_companion_radio_ble_dualboot`, Meshtastic env `seeed-xiao-s3-dualboot`.

```bash
# 1. Selector
cd ~/src/official-mesh/dualboot/selector
pio run -e heltec_v4              # or: pio run -e t_beam_supreme
                                  # or: pio run -e xiao_s3_wio

# 2. MeshCore
cd ~/src/MeshCore
pio run -e heltec_v4_companion_radio_ble_dualboot
# T-Beam: pio run -e T_Beam_S3_Supreme_SX1262_companion_radio_ble_dualboot
# XIAO:   pio run -e Xiao_S3_WIO_companion_radio_ble_dualboot

# 3. Official Mesh firmware
cd ~/src/official-mesh/firmware
pio run -e heltec-v4-dualboot
# T-Beam: pio run -e tbeam-s3-core-dualboot
# XIAO:   pio run -e seeed-xiao-s3-dualboot
```

Then full-chip flash. `--board` is required — pick the matching board
key from `slots.json`:

```bash
python3 ~/src/official-mesh/dualboot/scripts/flash_all.py \
    --board heltec_v4 --port /dev/cu.usbmodem*
# T-Beam: --board t_beam_supreme
# XIAO:   --board xiao_s3_wio
```

> The XIAO's native USB port only shows up as a serial device while an app
> is running. If the device is in the ROM loader (or freshly erased), hold
> **BOOT** and tap **RESET** to land in download mode first; esptool then
> talks to the USB-Serial-JTAG interface on the same connector.

`flash_all.py --help` shows partial-update flags:

* `--only 0` / `--only 1` / `--only selector` — refresh one slot (you can also pass the slot's configured name, e.g. `--only meshtastic`)
* `--reset-selection` — erase otadata so the menu shows on next boot
* `--slot0 <path>` / `--slot1 <path>` — point at custom firmware bins (overrides the defaults derived from `slots.json`)
* `--net-ota` — use the net-OTA partition layout (see below)

### Net-OTA: refresh a slot from your phone, no laptop

A compile-time-optional mode in which the selector itself accepts an
authenticated firmware push over WiFi+TCP and writes it to a chosen
`ota_X` partition. **Same operation as `--only`, just over the network.**
Otadata is untouched; the selector keeps boot ownership.

Three boards supported:
* **16 MB Heltec V4** (`heltec_v4_netota`): factory grows to 1 MB; OTA
  slots and filesystems unchanged. Comfortable headroom (~32%).
* **8 MB T-Beam Supreme** (`t_beam_supreme_netota`): factory grows to
  768 KB; OTA slots trim 64 KB each (3.125 MB ceiling — current
  Meshtastic builds are well under), filesystems trim 128 KB each
  (still 384 KB each — plenty for typical config + node DB). Tight
  on factory (~95% used in HMAC-only build); enabling Ed25519 leaves
  only ~12 KB headroom.
* **8 MB XIAO ESP32S3 + Wio-SX1262** (`xiao_s3_wio_netota`): shares the
  T-Beam's 8 MB net-OTA layout, so the same ~95% factory estimate
  applies — the XIAO build is slightly smaller (no PMU code), but don't
  bank on more than a few tens of KB. Headless-first: the AP path needs
  the expansion-board OLED to show the PSK, so use the STA/headless flow.

Build and flash:

```bash
cd ~/src/official-mesh/dualboot/selector
pio run -e heltec_v4_netota                                  # selector with WiFi+TCP listener
cd ~/src/official-mesh/firmware
pio run -e heltec-v4-dualboot                                # firmware (uses partitions_app_netota.csv)
cd ~/src/MeshCore
pio run -e heltec_v4_companion_radio_ble_dualboot            # firmware (uses partitions_app_netota.csv)
flash_all.py --net-ota --board heltec_v4 --port /dev/cu.usbmodem*

# 8 MB boards: swap in t_beam_supreme_netota / xiao_s3_wio_netota and build
# the firmware envs against partitions_app_8MB_netota.csv, then:
#   flash_all.py --net-ota --board xiao_s3_wio --port ...
```

> **Existing dualboot devices need a one-time full reflash to migrate** —
> `partitions_netota.csv` shifts every offset after `factory` by 0xA0000
> (factory grows from 384 KB to 1 MB to fit WiFi+lwIP). Both ota slots
> get erased.

The selector exposes net-OTA via three orthogonal compile-time axes:

| Flag | Picks |
|------|-------|
| `MDB_NET_OTA_AP` / `MDB_NET_OTA_STA` | softAP (proximate) and/or station (remote LAN) |
| `MDB_NET_OTA_HEADLESS` | NVS-flag trigger from firmware (no menu interaction) |
| `MDB_NET_OTA_AUTH_HMAC` | per-session/NVS PSK with HMAC-SHA256 over the bin |
| `MDB_NET_OTA_AUTH_ED25519` | Build-time-baked pubkey (`MDB_NET_OTA_PUBKEY_HEX="..."`); accepts bins signed by the matching private key. Adds ~25 KB. |

Default `[env:heltec_v4_netota]` enables AP+STA+headless+HMAC. Ed25519 is
wired but commented out — uncomment both lines in the env once you have a
keypair and a 64-char hex pubkey to bake in. The Ed25519 verifier is
[orlp/ed25519](https://github.com/orlp/ed25519) (Zlib license), pulled in
as a git submodule under `selector/lib/ed25519`. After cloning this repo:

```bash
git submodule update --init --recursive
```

Trigger paths:
* **Proximate (AP)** — from the selector menu, choose "Net flash...", pick
  the slot. OLED shows a per-session PSK; phone joins the AP, runs the
  pusher.
* **Headless (STA)** — on the running firmware, an admin/CLI command
  calls `mdb::set_net_flash_wifi(ssid, psk)` once, then
  `mdb::request_net_flash(ssid, psk, timeout_s)` whenever you want to
  open the listener window. Device reboots, joins the network, advertises
  itself as `mdb-XXYY.local` via mDNS, listens on TCP/5050. The CLIENT
  picks which slot to flash via the wire header, so a single trigger can
  flash both slots back-to-back.

#### Signing and pushing

Two helper scripts under `scripts/`:

`mdb_sign.py` wraps a firmware bin into the net-OTA wire format. HMAC mode
needs only stdlib; Ed25519 mode needs `pip install cryptography`.

```bash
# One-time: generate a keypair (Ed25519)
scripts/mdb_sign.py keygen --priv-out keys/release.priv --pub-out keys/release.pub
# → prints the 64-char hex pubkey to paste into MDB_NET_OTA_PUBKEY_HEX

# HMAC-signed bin (uses per-session PSK from OLED, or NVS-stored PSK)
scripts/mdb_sign.py sign --hmac --psk "AbCdEfGh" \
    --slot 1 --in firmware.bin --out firmware.signed

# Ed25519-signed bin (verifies against baked-in pubkey on the device)
scripts/mdb_sign.py sign --ed25519 --priv keys/release.priv \
    --slot 1 --in firmware.bin --out firmware.signed
```

`mdb_push.py` opens a TCP connection to the listener, streams the signed
bin, reads back a 1-byte status code mapping to a human-readable cause:

```bash
# AP mode: join "mdb-XXYY" with the PSK shown on the OLED, then:
scripts/mdb_push.py --host 192.168.4.1 --bin firmware.signed

# STA mode: device advertises itself via mDNS as mdb-XXYY.local
scripts/mdb_push.py --host mdb-abcd.local --bin firmware.signed
```

Status code reporting covers magic/version mismatches, slot-binding
violations, oversize payloads, mid-stream aborts, signature failures,
and OTA write/end errors — see the `STATUS_NAMES` dict in `mdb_push.py`
for the full mapping.

## Upgrading from rc2

Two behavior changes since rc2 are worth knowing before you flash:

**Slot order swapped.** `slots.json` now puts the Official Mesh
firmware at index 0 and MeshCore at index 1 (was the reverse). The
selector stores the last selection as a raw integer in `nvs_sel`, so a
device upgraded from rc2 with auto-boot enabled will silently boot the
*other* firmware until you re-pick from the menu. Fix is a one-time
menu interaction post-upgrade; no data is lost.

**Net-OTA partition layout (opt-in).** If you switch to the
`heltec_v4_netota` / `t_beam_supreme_netota` selector envs, the
partition table changes — factory grows to 1 MB on 16 MB boards and
768 KB on 8 MB boards, shifting every offset after `factory`. Both ota
slots get erased in the process, so you'll need to re-flash both
firmwares as well. `flash_all.py --net-ota --board <board>` does the
full migration in one shot. Devices that stay on the non-net-OTA
build need none of this.

## Configuring what's in each slot

[`slots.json`](slots.json) is the single source of truth for slot names
(displayed on the OLED) and default firmware bin paths. To swap MeshCore
and Meshtastic between slots, just reorder the two entries for your
board, then rebuild and re-flash the selector:

```bash
cd ~/src/official-mesh/dualboot/selector && pio run -e heltec_v4
python3 ~/src/official-mesh/dualboot/scripts/flash_all.py --board heltec_v4 \
    --port /dev/cu.usbmodem* --only selector
```

To put two different Meshtastic builds on one device (e.g. for A/B
comparison), edit `slots.json`:

```json
"heltec_v4": [
  {"name": "MT 2.5", "build": {"kind": "literal", "path": "~/builds/mt-2.5.bin"}},
  {"name": "MT 2.6", "build": {"kind": "meshtastic", "env": "heltec-v4-dualboot"}}
]
```

Heads-up: both Meshtastics will share the `spiffs` and `nvs` partitions,
so they'll see the same config / channels / node DB. Same-major versions
are fine; cross-major can rewrite the on-disk schema in a way the older
side doesn't fully understand.

> **Don't use in-firmware OTA on a dualboot device.** Standard A/B OTA
> assumes `ota_0` and `ota_1` are two slots of the same firmware. Here
> they hold two different firmwares, so an OTA from one would
> overwrite the other and leave `otadata` pointing at the wrong slot —
> after which the menu's labels no longer match what's on flash.
> Update via `--only` over USB, or via the net-OTA listener in the
> selector (see above). The selector-mediated path is safe because it
> writes directly to a chosen ota_X without touching otadata; the
> in-firmware OTA path is the one to avoid. See PORTING.md for details.

## Firmware-side changes

The dualboot envs are additive — your existing build envs are untouched.

**MeshCore** (`variants/heltec_v4/platformio.ini`) adds env
`heltec_v4_companion_radio_ble_dualboot`. Source patches:

* `src/helpers/esp32/MeshDualbootHook.h` — new file, the cooperative hook
* `examples/companion_radio/main.cpp` — `#include` + `mdb::check_return_to_selector()` at top of `setup()`
* `examples/companion_radio/main.cpp` — SPIFFS uses the `mc_spiffs` partition label when `MC_SPIFFS_LABEL` is defined
* All patches are guarded by `#ifdef MDB_DUALBOOT` / `#ifdef MC_SPIFFS_LABEL`, so non-dualboot builds are unaffected.

The dualboot env also overrides `BLE_NAME_PREFIX="MC_"` so the BLE
advertising name distinguishes the firmware on the same hardware MAC.

**Meshtastic** (`variants/esp32s3/heltec_v4/platformio.ini`) adds env
`heltec-v4-dualboot`. Source patches:

* `src/platform/esp32/MeshDualbootHook.h` — new file
* `src/main.cpp` — `#include` + `mdb::check_return_to_selector()` at top of `setup()`
* All patches `#ifdef MDB_DUALBOOT`-guarded.

**XIAO ESP32S3 + Wio-SX1262** — same two patches, three board-specific
defines (full env snippets in [PORTING.md](PORTING.md#worked-example-seeed-xiao-esp32s3--wio-sx1262-8-mb-no-display)):

* MeshCore (`variants/xiao_s3_wio/platformio.ini`) adds env
  `Xiao_S3_WIO_companion_radio_ble_dualboot`, extending
  `env:Xiao_S3_WIO_companion_radio_ble` with `-D MDB_DUALBOOT=1`,
  `-D MDB_BUTTON_PIN=21`, `-D MC_SPIFFS_LABEL="mc_spiffs"`,
  `-D BLE_NAME_PREFIX="MC_"`, and `partitions_app_8MB.csv`.
* Meshtastic (`variants/esp32s3/seeed_xiao_s3/platformio.ini`) adds env
  `seeed-xiao-s3-dualboot`, extending `env:seeed-xiao-s3` with
  `-D MDB_DUALBOOT=1`, `-D MDB_BUTTON_PIN=21`, and
  `partitions_app_8MB.csv`.

`MDB_BUTTON_PIN=21` is load-bearing, not cosmetic: the hook defaults to
GPIO0, and on the XIAO the user button is GPIO21 (GPIO0 is the BOOT strap
pin). Without the override, hold-to-return watches the wrong button.

### Seeed XIAO ESP32S3 + Wio-SX1262 notes

The bare kit is the first supported target without a display, so the
selector runs in its LED-blink mode unless you add a panel:

* **Display is auto-detected.** The selector probes `0x3C` on D4/D5
  (SDA=GPIO5, SCL=GPIO6) — the pins the XIAO Expansion Board's 128×64
  SSD1306 sits on. Nothing answers → LED mode. Clip the expansion board
  on and the full OLED menu appears, same binary, no rebuild.
* **LED-blink menu** (green LED on GPIO48, active HIGH): 1 blink = slot 0
  (`Official`), 2 blinks = slot 1 (`MeshCore`), repeating roughly every
  2 s. Tap to cycle, hold ≥0.5 s to boot into the blinking slot. The
  auto-boot countdown is disabled in this mode, so an unattended node
  never picks a firmware by itself.
* **Button is GPIO21, not the BOOT strap.** The Wio-SX1262 module's user
  button sits on GPIO21 with its own 10K pull-up, so holding it through a
  power-cycle is safe — the chip still runs the selector instead of
  dropping into the ROM serial loader (which is exactly what GPIO0 would
  do).
* **Return-to-selector:** hold the user button (GPIO21) for ~2 s during
  the firmware's startup window, or double-tap the XIAO's RESET button
  within ~3 s. Both need the firmware-side hook described above.
* **The selector never touches the radio.** The SX1262 hangs off SPI1
  (SCK 7 / MISO 8 / MOSI 9 / CS 41 / RST 42 / BUSY 40 / DIO1 39 /
  RXEN 38, TX/RX switch driven by DIO2). Leaving it idle for the ~1 s the
  selector runs is fine — no FEM LDO to keep powered, unlike the Heltec.
* **8 MB flash**, so it uses the same `partitions_8MB.csv` layout as the
  T-Beam Supreme (3.1875 MB per OTA slot).
* **Never use in-firmware OTA** here either; the generic warning above
  applies unchanged. With no display the selector-mediated net-OTA path
  is the practical field-update route — build `xiao_s3_wio_netota` and
  drive it from the firmware's headless/STA trigger.

## Adding more boards

See [PORTING.md](PORTING.md) for the minimum requirements, the full
list of `MDB_*` configuration macros, and a step-by-step recipe for
adding a new board (or a new firmware pair). T-Beam Supreme support
in this repo is itself a worked example of the procedure; the XIAO port
is the worked example for a headless (no-display) board.

## Verifying releases

Signed git tags and release artifacts are signed by:

    The Official Mesh Admin <officialmeshadmin@proton.me>
    GPG fingerprint: 9A18 814D 74A6 3138 9F95  6EA0 5F8D 7A5E ED20 3343

The public key is in [`KEYS`](KEYS) at the repo root, or fetch it from a keyserver:

```bash
gpg --keyserver hkps://keys.openpgp.org --recv-keys 9A18814D74A631389F956EA05F8D7A5EED203343
```

Always verify the fingerprint matches the value above before importing.

```bash
gpg --verify official-mesh-dualboot-<ver>.tar.gz.asc official-mesh-dualboot-<ver>.tar.gz   # release artifact
git verify-tag v<ver>                                                                       # signed git tag
```

## Contributing

Pull requests are welcome and are governed by the policies maintained centrally
in [`official-mesh/meshbot`](https://github.com/official-mesh/meshbot):

- [Contributor License Agreement](https://github.com/official-mesh/meshbot/blob/main/CLA.md) — sign by including the agreement statement as a comment on your first PR (see the **How to sign** section in `CLA.md`).
- [Code of Conduct](https://github.com/official-mesh/meshbot/blob/main/CODE_OF_CONDUCT.md) — applies to participation here. Report unacceptable behavior to officialmeshadmin@proton.me.

## License

Copyright (C) 2026 The Official Mesh Admin

This program is free software: you can redistribute it and/or modify it under the terms of version 3 of the GNU Affero General Public License as published by the Free Software Foundation. See [`LICENSE`](LICENSE) for the full text.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

The selector source is a clean-room implementation; the reference for the
boot-partition-switch pattern is the Apache-2.0
[esp32-graphical-bootloader](https://github.com/georgik/esp32-graphical-bootloader).

## Status

Hardware-verified on Heltec V4 (2026-05-02) and LilyGo T-Beam Supreme
(2026-05-03): selector renders correctly, button cycle + long-press
commit, OTA partition switch into both slots, and PRG-long-press
return-to-selector all work on both boards. The double-tap-RST return
path is currently broken on the Heltec V4 test unit (pre-existing, not
caused by recent changes); use PRG-long-press during the firmware's
startup window instead.

**XIAO ESP32S3 + Wio-SX1262: not yet hardware-verified.** The selector
builds for `xiao_s3_wio` / `xiao_s3_wio_netota`, the partition layout is
the T-Beam-verified 8 MB table, and the pin map is taken from the
upstream MeshCore `xiao_s3_wio` and Meshtastic `seeed_xiao_s3` variants
(button GPIO21, LED GPIO48, I²C 5/6). What still needs a real device:
LED-blink gestures, the PRG-equivalent return to selector, and the
`seeed-xiao-s3-dualboot` firmware env. Treat first flash as bring-up and
keep a USB cable handy.
