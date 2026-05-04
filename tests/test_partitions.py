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

# (full_csv, app_csv, flash_size_bytes)
PAIRS = [
    ("partitions.csv",     "partitions_app.csv",     16 * 1024 * 1024),
    ("partitions_8MB.csv", "partitions_app_8MB.csv",  8 * 1024 * 1024),
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
    for full, app, flash in PAIRS:
        failures += _check(full, app, flash)
    assert not failures, "\n".join(failures)


def test_app_slots_have_room_for_known_firmware_sizes():
    """Sanity: each ota slot should be ≥ a target headroom over our typical
    build sizes. Catches the bug where someone shrinks ota_0/ota_1 below
    what Meshtastic actually compiles to."""
    HEADROOM = 0x100000  # 1 MB headroom over current builds
    OBSERVED = {
        # board → (mc_bytes, mt_bytes) from recent builds
        "16MB": (1_300_000, 2_400_000),
        "8MB":  (1_300_000, 2_400_000),
    }
    for full_name, _, flash in PAIRS:
        key = "16MB" if flash == 16 * 1024 * 1024 else "8MB"
        mc_size, mt_size = OBSERVED[key]
        parts = {p.subtype: p for p in parse(REPO / full_name)}
        assert parts["ota_0"].size >= mc_size + HEADROOM, (
            f"{full_name} ota_0 too small: 0x{parts['ota_0'].size:x} < "
            f"0x{mc_size + HEADROOM:x}"
        )
        assert parts["ota_1"].size >= mt_size + HEADROOM, (
            f"{full_name} ota_1 too small: 0x{parts['ota_1'].size:x} < "
            f"0x{mt_size + HEADROOM:x}"
        )


if __name__ == "__main__":
    fails: list[str] = []
    for f, a, fl in PAIRS:
        fails += _check(f, a, fl)
    if fails:
        for x in fails: print(x)
        sys.exit(1)
    print(f"OK — {len(PAIRS)} CSV pair(s) validated")
