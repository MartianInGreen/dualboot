# mesh-dualboot

Run **MeshCore** or **Meshtastic** on the same ESP32-S3 device, picked at boot
from an OLED menu — no reflashing.

Supported targets (both hardware-verified):
- **Heltec WiFi LoRa 32 v4** (16 MB flash, SSD1315 OLED).
- **LilyGo T-Beam Supreme** (8 MB flash).

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

`partitions_8MB.csv` (8 MB — T-Beam Supreme):

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
- T-Beam Supreme (8 MB): selector env `t_beams3_supreme`, MeshCore env `T_Beam_S3_Supreme_SX1262_companion_radio_ble_dualboot`, Meshtastic env `tbeam-s3-core-dualboot`.

```bash
# 1. Selector
cd ~/src/official-mesh/dualboot/selector
pio run -e heltec_v4              # or: pio run -e t_beams3_supreme

# 2. MeshCore
cd ~/src/MeshCore
pio run -e heltec_v4_companion_radio_ble_dualboot
# T-Beam: pio run -e T_Beam_S3_Supreme_SX1262_companion_radio_ble_dualboot

# 3. Official Mesh firmware
cd ~/src/official-mesh/firmware
pio run -e heltec-v4-dualboot
# T-Beam: pio run -e tbeam-s3-core-dualboot
```

Then full-chip flash. `--board` is required — pick the matching board
key from `slots.json`:

```bash
python3 ~/src/official-mesh/dualboot/scripts/flash_all.py \
    --board heltec_v4 --port /dev/cu.usbmodem*
# T-Beam: --board t_beams3_supreme
```

`flash_all.py --help` shows partial-update flags:

* `--only 0` / `--only 1` / `--only selector` — refresh one slot (you can also pass the slot's configured name, e.g. `--only meshtastic`)
* `--reset-selection` — erase otadata so the menu shows on next boot
* `--slot0 <path>` / `--slot1 <path>` — point at custom firmware bins (overrides the defaults derived from `slots.json`)

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
> Update via `--only` over USB instead. See PORTING.md for details.

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

## Adding more boards

See [PORTING.md](PORTING.md) for the minimum requirements, the full
list of `MDB_*` configuration macros, and a step-by-step recipe for
adding a new board (or a new firmware pair). T-Beam Supreme support
in this repo is itself a worked example of the procedure.

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
