# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 The Official Mesh Admin

"""Validate every partitions*.csv in the repo.

Run:  pytest tests/  (or)  python -m pytest tests/test_partitions.py -v
Standalone:  python tests/test_partitions.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _partitions import parse, validate, diff_pair  # noqa: E402

REPO = Path(__file__).resolve().parent.parent

# Recent build sizes from the active firmware fork; both ports compile to
# similar ranges. The Meshtastic floor (2.6 MB) matches the design margin
# cited in partitions_8MB_netota.csv's own header comment, which is the
# tightest layout this repo ships.
_MC_TYPICAL = 1_300_000
_MT_TYPICAL = 2_600_000

# Per-pair design margins. The 16 MB layouts have luxurious slack and assert
# 1 MB of headroom on each slot; the 8 MB layouts trade slot space for
# selector / net-OTA room and assert the design floor (~512 KB) on ota_1
# explicitly.
#
# (full_csv, app_csv, flash_bytes, ota0_headroom, ota1_headroom)
PAIRS = [
    ("partitions.csv",            "partitions_app.csv",            16 * 1024 * 1024, 0x100000, 0x100000),
    ("partitions_8MB.csv",        "partitions_app_8MB.csv",         8 * 1024 * 1024, 0x100000, 0x80000),
    ("partitions_netota.csv",     "partitions_app_netota.csv",     16 * 1024 * 1024, 0x100000, 0x100000),
    ("partitions_8MB_netota.csv", "partitions_app_8MB_netota.csv",  8 * 1024 * 1024, 0x100000, 0x80000),
]


def _check(full_name: str, app_name: str, flash_size: int) -> list[str]:
    full = parse(REPO / full_name)
    app  = parse(REPO / app_name)
    errs: list[str] = []
    errs += [f"{full_name}: {e}" for e in validate(full, flash_size, allow_factory_missing=False)]
    errs += [f"{app_name}: {e}"  for e in validate(app,  flash_size, allow_factory_missing=True)]
    errs += [f"pair({full_name},{app_name}): {e}" for e in diff_pair(full, app)]
    return errs


def test_partition_pairs_are_well_formed():
    failures: list[str] = []
    for full, app, flash, _h0, _h1 in PAIRS:
        failures += _check(full, app, flash)
    assert not failures, "\n".join(failures)


def test_app_slots_have_room_for_known_firmware_sizes():
    """Each ota slot must fit a current build plus a per-layout headroom.
    Catches the bug where someone shrinks ota_0/ota_1 below what either
    firmware actually compiles to, even on the tighter 8 MB / net-OTA layouts."""
    for full_name, _app, _flash, ota0_headroom, ota1_headroom in PAIRS:
        parts = {p.subtype: p for p in parse(REPO / full_name)}
        ota0_required = _MC_TYPICAL + ota0_headroom
        ota1_required = _MT_TYPICAL + ota1_headroom
        assert parts["ota_0"].size >= ota0_required, (
            f"{full_name} ota_0 too small: 0x{parts['ota_0'].size:x} < "
            f"0x{ota0_required:x} (MeshCore {_MC_TYPICAL:,} B + "
            f"0x{ota0_headroom:x} headroom)"
        )
        assert parts["ota_1"].size >= ota1_required, (
            f"{full_name} ota_1 too small: 0x{parts['ota_1'].size:x} < "
            f"0x{ota1_required:x} (Meshtastic {_MT_TYPICAL:,} B + "
            f"0x{ota1_headroom:x} headroom)"
        )


def test_netota_factory_grew_for_listener():
    """The net-OTA selector adds WiFi + lwIP + auth + UI to the boot menu, so
    the factory partition must be larger than its non-net-OTA sibling. Guards
    against the regression where someone copies the small factory size from
    partitions.csv onto the net-OTA layout and silently breaks the listener."""
    siblings = [
        ("partitions.csv",     "partitions_netota.csv"),
        ("partitions_8MB.csv", "partitions_8MB_netota.csv"),
    ]
    for plain_name, netota_name in siblings:
        plain   = {p.subtype: p for p in parse(REPO / plain_name)}
        netota  = {p.subtype: p for p in parse(REPO / netota_name)}
        assert netota["factory"].size > plain["factory"].size, (
            f"{netota_name}: factory 0x{netota['factory'].size:x} should exceed "
            f"{plain_name} factory 0x{plain['factory'].size:x} to fit the "
            f"net-OTA listener"
        )


def test_netota_app_csv_matches_firmware_repo():
    """The firmware repo vendors a copy of each app-view net-OTA CSV at
    `firmware/variants/esp32s3/dualboot/`. Slot offsets and sizes must match —
    drift breaks net-OTA flashing because the pushed bin lands at the wrong
    address. Skipped silently when the sibling firmware checkout isn't present."""
    firmware_dir = REPO.parent / "firmware" / "variants" / "esp32s3" / "dualboot"
    if not firmware_dir.exists():
        return
    for app_name in (
        "partitions_app_netota.csv",
        "partitions_app_8MB_netota.csv",
    ):
        local = parse(REPO / app_name)
        vendored = parse(firmware_dir / app_name)
        assert local == vendored, (
            f"{app_name}: drift between dualboot/{app_name} and "
            f"firmware/variants/esp32s3/dualboot/{app_name}; both must agree "
            f"on slot offsets and sizes for net-OTA flashing to land correctly"
        )


if __name__ == "__main__":
    fails: list[str] = []
    for f, a, fl, _h0, _h1 in PAIRS:
        fails += _check(f, a, fl)
    if fails:
        for x in fails: print(x)
        sys.exit(1)
    print(f"OK — {len(PAIRS)} CSV pair(s) validated")
