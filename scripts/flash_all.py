#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 The Official Mesh Admin

# mesh-dualboot — full-chip flash for ESP32-S3 dual-firmware setups.
#
# Boards supported:
#   heltec_v4       — Heltec WiFi LoRa 32 v4, 16 MB
#   t_beam_supreme  — LilyGo T-Beam Supreme, 8 MB
#
# Slot assignments (names + default firmware bin paths) come from
# ../slots.json. Override per-flash with --slot0 <path> / --slot1 <path>.
#
# Usage:
#   flash_all.py --board heltec_v4 --port /dev/cu.usbmodem*
#   flash_all.py --board heltec_v4 --port ... --only 0
#   flash_all.py --board heltec_v4 --port ... --only 1
#   flash_all.py --board heltec_v4 --port ... --slot1 ~/builds/mt-2.6.bin
#   flash_all.py --board heltec_v4 --port ... --reset-selection

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
HOME = Path.home()
SLOTS_JSON = REPO_ROOT / "slots.json"


def _resolve_meshtastic(env):
    d = REPO_ROOT.parent / f"firmware/.pio/build/{env}"
    if not d.exists():
        return str(d / "firmware.bin")
    candidates = sorted(
        d.glob(f"firmware-{env}-*.bin"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    for c in candidates:
        if ".factory" not in c.name:
            return str(c)
    return str(d / "firmware.bin")


def _resolve_meshcore(env):
    return str(HOME / f"src/MeshCore/.pio/build/{env}/firmware.bin")


def _resolve_build(build):
    """Turn a slots.json 'build' object into a path string."""
    kind = build.get("kind")
    if kind == "meshcore":
        return _resolve_meshcore(build["env"])
    if kind == "meshtastic":
        return _resolve_meshtastic(build["env"])
    if kind == "literal":
        return os.path.expanduser(build["path"])
    raise SystemExit(f"slots.json: unknown build kind {kind!r}")


# Per-board flash layout. Offsets MUST match the corresponding partitions CSV.
BOARDS = {
    "heltec_v4": {
        "flash_size": "16MB",
        "selector_env": "heltec_v4",
        "offsets": {
            "bootloader": 0x000000,
            "partitions": 0x008000,
            "otadata":    0x010000,
            "factory":    0x020000,
            "ota_0":      0x080000,
            "ota_1":      0x5C0000,
        },
        "slot_size": {
            "factory": 0x60000,
            "ota_0":   0x540000,
            "ota_1":   0x540000,
        },
    },
    "t_beam_supreme": {
        "flash_size": "8MB",
        "selector_env": "t_beam_supreme",
        "offsets": {
            "bootloader": 0x000000,
            "partitions": 0x008000,
            "otadata":    0x010000,
            "factory":    0x020000,
            "ota_0":      0x080000,
            "ota_1":      0x3B0000,
        },
        "slot_size": {
            "factory": 0x60000,
            "ota_0":   0x330000,
            "ota_1":   0x330000,
        },
    },
}

# Net-OTA layout overlays. When --net-ota is passed, these override the
# matching BOARDS entry. Offsets MUST match the corresponding
# partitions_*_netota.csv file.
NETOTA_OVERRIDES = {
    "heltec_v4": {
        "selector_env": "heltec_v4_netota",
        "offsets": {
            "bootloader": 0x000000,
            "partitions": 0x008000,
            "otadata":    0x010000,
            "factory":    0x020000,
            "ota_0":      0x120000,
            "ota_1":      0x660000,
        },
        "slot_size": {
            "factory": 0x100000,
            "ota_0":   0x540000,
            "ota_1":   0x540000,
        },
    },
    "t_beam_supreme": {
        "selector_env": "t_beam_supreme_netota",
        "offsets": {
            "bootloader": 0x000000,
            "partitions": 0x008000,
            "otadata":    0x010000,
            "factory":    0x020000,
            "ota_0":      0x0E0000,
            "ota_1":      0x400000,
        },
        "slot_size": {
            "factory": 0xC0000,
            "ota_0":   0x320000,
            "ota_1":   0x320000,
        },
    },
}


def load_slots(board):
    with open(SLOTS_JSON, "r") as f:
        cfg = json.load(f)
    slots = cfg.get(board)
    if not slots:
        sys.exit(f"slots.json: no entry for board {board!r}")
    if len(slots) != 2:
        sys.exit(f"slots.json[{board!r}]: expected 2 slots, got {len(slots)}")
    return slots


def default_paths(cfg, slots):
    sel_dir = REPO_ROOT / f"selector/.pio/build/{cfg['selector_env']}"
    return {
        "bootloader": str(sel_dir / "bootloader.bin"),
        "partitions": str(sel_dir / "partitions.bin"),
        "factory":    str(sel_dir / "firmware.bin"),
        "ota_0":      _resolve_build(slots[0]["build"]),
        "ota_1":      _resolve_build(slots[1]["build"]),
    }


def find_esptool():
    candidates = [
        "esptool.py", "esptool",
        str(HOME / ".platformio/penv/bin/esptool.py"),
        str(HOME / ".platformio/penv/bin/esptool"),
    ]
    for c in candidates:
        if shutil.which(c) or os.path.exists(c):
            return c
    sys.exit("esptool.py not found; install with `pip install esptool` or use PlatformIO's penv.")


def run_esptool(port, baud, args, dry_run):
    esptool = find_esptool()
    cmd = [
        esptool, "--chip", "esp32s3",
        "--port", port, "--baud", str(baud),
        "--before", "default_reset", "--after", "hard_reset",
    ] + args
    print("$", " ".join(cmd))
    if dry_run:
        return
    subprocess.run(cmd, check=True)


def write_flash(port, baud, flash_size, pairs, dry_run):
    args = ["write_flash", "-z", "--flash_mode", "dio", "--flash_size", flash_size]
    for off, p in pairs:
        args += [f"0x{off:x}", p]
    run_esptool(port, baud, args, dry_run)


def erase_region(port, baud, offset, size, dry_run):
    run_esptool(port, baud, ["erase_region", f"0x{offset:x}", f"0x{size:x}"], dry_run)


def assert_size(label, path, max_size, dry_run=False):
    if not os.path.exists(path):
        if dry_run:
            print(f"  {label:<11} (would-be) {path} (not built; size check skipped in --dry-run)")
            return
        sys.exit(f"{label}: not found at {path} — build it first or pass --{label.lower()} <path>")
    actual = os.path.getsize(path)
    if actual > max_size:
        sys.exit(f"{label}: {path} is {actual} bytes, exceeds slot size {max_size}")
    pct = actual * 100 // max_size
    print(f"  {label:<11} {actual:>9,} / {max_size:>9,} bytes ({pct}%)  {path}")


def parse_only(value, slot_names):
    """Map --only value (0/1/selector or a slot name or compat alias) to the
    canonical key: 'slot0', 'slot1', or 'selector'."""
    if value is None:
        return None
    v = value.lower()
    if v in ("selector", "factory"):
        return "selector"
    if v in ("0", "slot0", "ota0", "ota_0"):
        return "slot0"
    if v in ("1", "slot1", "ota1", "ota_1"):
        return "slot1"
    # Match by configured slot name (case-insensitive).
    for i, n in enumerate(slot_names):
        if v == n.lower():
            return f"slot{i}"
    # Back-compat aliases that pre-date slots.json.
    if v == "meshcore":
        return "slot0"
    if v == "meshtastic":
        return "slot1"
    sys.exit(f"--only: unknown value {value!r}; expected 0, 1, selector, or one of {slot_names}")


def main():
    ap = argparse.ArgumentParser(description="mesh-dualboot full-chip flasher")
    ap.add_argument("--board", required=True, choices=list(BOARDS.keys()),
                    help="target board")
    ap.add_argument("--port", required=True, help="serial port")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--only",
                    help="flash only one slot: 0, 1, selector, or a configured slot name")
    ap.add_argument("--net-ota", dest="net_ota", action="store_true",
                    help="use the net-OTA partition layout (factory grown to 1 MB; "
                         "selector built with WiFi+TCP listener). Only supported on "
                         "boards in NETOTA_OVERRIDES.")
    ap.add_argument("--reset-selection", action="store_true",
                    help="erase otadata so the selector menu shows on next boot")
    ap.add_argument("--bootloader")
    ap.add_argument("--partitions")
    ap.add_argument("--factory")
    ap.add_argument("--slot0", help="path to firmware bin for slot 0 (ota_0)")
    ap.add_argument("--slot1", help="path to firmware bin for slot 1 (ota_1)")
    # Back-compat aliases.
    ap.add_argument("--meshcore", help=argparse.SUPPRESS)
    ap.add_argument("--meshtastic", help=argparse.SUPPRESS)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    cfg = BOARDS[args.board]
    if args.net_ota:
        if args.board not in NETOTA_OVERRIDES:
            sys.exit(f"--net-ota: board {args.board!r} does not support net-OTA "
                     f"(8 MB boards would have to shrink OTA slots; "
                     f"see partitions_netota.csv for the supported 16 MB layout)")
        cfg = {**cfg, **NETOTA_OVERRIDES[args.board]}
    slots = load_slots(args.board)
    slot_names = [s["name"] for s in slots]
    offsets = cfg["offsets"]
    slot_size = cfg["slot_size"]
    flash_size = cfg["flash_size"]
    paths = default_paths(cfg, slots)

    # Override defaults from CLI.
    if args.bootloader: paths["bootloader"] = args.bootloader
    if args.partitions: paths["partitions"] = args.partitions
    if args.factory:    paths["factory"]    = args.factory
    if args.slot0:      paths["ota_0"]      = args.slot0
    if args.slot1:      paths["ota_1"]      = args.slot1
    # Back-compat: --meshcore/--meshtastic mirror --slot0/--slot1.
    if args.meshcore:   paths["ota_0"]      = args.meshcore
    if args.meshtastic: paths["ota_1"]      = args.meshtastic

    only = parse_only(args.only, slot_names)

    if args.reset_selection:
        erase_region(args.port, args.baud, offsets["otadata"], 0x2000, args.dry_run)
        print("otadata erased — selector will run on next boot.")
        return

    pairs = []

    if only == "slot0":
        assert_size(slot_names[0], paths["ota_0"], slot_size["ota_0"], args.dry_run)
        pairs.append((offsets["ota_0"], paths["ota_0"]))
    elif only == "slot1":
        assert_size(slot_names[1], paths["ota_1"], slot_size["ota_1"], args.dry_run)
        pairs.append((offsets["ota_1"], paths["ota_1"]))
    elif only == "selector":
        assert_size("selector", paths["factory"], slot_size["factory"], args.dry_run)
        pairs.append((offsets["factory"], paths["factory"]))
    else:
        # Full flash. assert_size handles --dry-run by skipping the existence
        # and size checks so the command line can be inspected without first
        # building all artifacts.
        for label, path, slot in [
            ("bootloader",   paths["bootloader"], None),
            ("partitions",   paths["partitions"], None),
            ("selector",     paths["factory"],    "factory"),
            (slot_names[0],  paths["ota_0"],      "ota_0"),
            (slot_names[1],  paths["ota_1"],      "ota_1"),
        ]:
            if slot:
                assert_size(label, path, slot_size[slot], args.dry_run)
            elif not args.dry_run:
                if not os.path.exists(path):
                    sys.exit(f"{label}: not found at {path} — build it first or pass --{label.lower()} <path>")
        pairs = [
            (offsets["bootloader"], paths["bootloader"]),
            (offsets["partitions"], paths["partitions"]),
            (offsets["factory"],    paths["factory"]),
            (offsets["ota_0"],      paths["ota_0"]),
            (offsets["ota_1"],      paths["ota_1"]),
        ]
        erase_region(args.port, args.baud, offsets["otadata"], 0x2000, args.dry_run)

    write_flash(args.port, args.baud, flash_size, pairs, args.dry_run)
    print("Done. Power-cycle the device.")


if __name__ == "__main__":
    main()
