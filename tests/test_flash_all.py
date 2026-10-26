# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 The Official Mesh Admin

"""Tests for scripts/flash_all.py — BOARDS vs CSV consistency, argparse."""
from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _partitions import parse  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "scripts" / "flash_all.py"


def _load_flash_all():
    spec = importlib.util.spec_from_file_location("flash_all", SCRIPT)
    mod = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


# Each board's full partition CSV (the one flashed at 0x8000, with factory).
BOARD_TO_FULL_CSV = {
    "heltec_v4":      "partitions.csv",
    "t_beam_supreme": "partitions_8MB.csv",
    "xiao_s3_wio":    "partitions_8MB.csv",
}

# Each board's full net-OTA partition CSV, used when --net-ota is passed.
BOARD_TO_NETOTA_CSV = {
    "heltec_v4":      "partitions_netota.csv",
    "t_beam_supreme": "partitions_8MB_netota.csv",
    "xiao_s3_wio":    "partitions_8MB_netota.csv",
}


def _assert_layout_matches_csv(board, cfg, csv_name):
    csv = REPO / csv_name
    parts = {p.subtype: p for p in parse(csv)}
    # Map our slot keys to partition subtypes.
    mapping = {
        "factory": "factory",
        "ota_0":   "ota_0",
        "ota_1":   "ota_1",
        "otadata": "ota",     # subtype is just "ota" for an otadata partition
    }
    for slot, sub in mapping.items():
        assert sub in parts, f"{csv.name}: missing subtype {sub!r}"
        assert cfg["offsets"][slot] == parts[sub].offset, (
            f"{board}.offsets[{slot!r}]=0x{cfg['offsets'][slot]:x} "
            f"!= csv 0x{parts[sub].offset:x} ({csv.name})"
        )
    for slot in ("factory", "ota_0", "ota_1"):
        assert cfg["slot_size"][slot] == parts[slot].size, (
            f"{board}.slot_size[{slot!r}]=0x{cfg['slot_size'][slot]:x} "
            f"!= csv 0x{parts[slot].size:x} ({csv.name})"
        )


def test_boards_offsets_match_csv():
    mod = _load_flash_all()
    for board, cfg in mod.BOARDS.items():
        _assert_layout_matches_csv(board, cfg, BOARD_TO_FULL_CSV[board])


def test_netota_overrides_match_csv():
    """Every board flash_all knows about must have a net-OTA overlay whose
    offsets/sizes agree with the matching partitions_*_netota.csv."""
    mod = _load_flash_all()
    for board in mod.BOARDS:
        assert board in mod.NETOTA_OVERRIDES, (
            f"board {board!r} has no NETOTA_OVERRIDES entry"
        )
        assert board in BOARD_TO_NETOTA_CSV, (
            f"no net-OTA CSV mapped for board {board!r}"
        )
        _assert_layout_matches_csv(board, mod.NETOTA_OVERRIDES[board],
                                   BOARD_TO_NETOTA_CSV[board])


def test_dry_run_full_flash_emits_expected_offsets():
    """The --dry-run output should write the bootloader at 0x0, partitions
    at 0x8000, then factory + ota_0 + ota_1 at the per-board offsets."""
    out = subprocess.run(
        [sys.executable, str(SCRIPT),
         "--board", "heltec_v4",
         "--port", "/dev/null",
         "--dry-run"],
        capture_output=True, text=True, check=True,
    ).stdout
    # The full-flash command line should mention each expected offset.
    for off in ("0x0", "0x8000", "0x20000", "0x80000", "0x5c0000"):
        assert off in out, f"expected offset {off} missing from dry-run output:\n{out}"
    # And the otadata erase that precedes it.
    assert "erase_region 0x10000 0x2000" in out or "erase-region 0x10000 0x2000" in out


def test_dry_run_only_meshtastic_writes_just_one_offset():
    out = subprocess.run(
        [sys.executable, str(SCRIPT),
         "--board", "heltec_v4",
         "--port", "/dev/null",
         "--only", "meshtastic",
         "--dry-run"],
        capture_output=True, text=True, check=True,
    ).stdout
    assert "0x5c0000" in out
    # Should NOT erase otadata in --only mode (would lose user's last selection).
    assert "erase_region" not in out and "erase-region" not in out
    # Should NOT touch the bootloader / selector / ota_0 offsets.
    for off in ("0x0", "0x8000", "0x20000", "0x80000"):
        assert f" {off} " not in out, f"unexpected offset {off} in --only meshtastic dry-run"


def test_only_by_slot_index():
    """--only 0 / --only 1 are the slot-generic equivalents of the legacy
    --only meshcore / --only meshtastic aliases."""
    out0 = subprocess.run(
        [sys.executable, str(SCRIPT), "--board", "heltec_v4",
         "--port", "/dev/null", "--only", "0", "--dry-run"],
        capture_output=True, text=True, check=True,
    ).stdout
    assert "0x80000" in out0
    assert "0x5c0000" not in out0

    out1 = subprocess.run(
        [sys.executable, str(SCRIPT), "--board", "heltec_v4",
         "--port", "/dev/null", "--only", "1", "--dry-run"],
        capture_output=True, text=True, check=True,
    ).stdout
    assert "0x5c0000" in out1
    # Guard against a regression where --only 1 also writes ota_0 at 0x80000.
    # We check space-padded so that an incidental "0x80000" inside a path
    # (unlikely but possible) doesn't false-positive — the offset on the
    # esptool command line is always surrounded by spaces.
    assert " 0x80000 " not in out1


def test_slot_path_overrides_default():
    """--slot1 <path> should redirect ota_1 to the given path."""
    fake = "/tmp/mesh-dualboot-test-slot1.bin"
    Path(fake).write_bytes(b"\x00" * 1024)
    try:
        out = subprocess.run(
            [sys.executable, str(SCRIPT), "--board", "heltec_v4",
             "--port", "/dev/null", "--only", "1",
             "--slot1", fake, "--dry-run"],
            capture_output=True, text=True, check=True,
        ).stdout
        assert fake in out, f"expected --slot1 path {fake!r} in dry-run output:\n{out}"
    finally:
        Path(fake).unlink(missing_ok=True)


def test_slots_json_round_trip():
    """slots.json must define exactly 2 slots for every board flash_all knows
    about, so the generic --only handling has names for both."""
    import json as _json
    cfg = _json.loads((REPO / "slots.json").read_text())
    mod = _load_flash_all()
    for board in mod.BOARDS:
        assert board in cfg, f"slots.json missing board {board!r}"
        slots = cfg[board]
        assert isinstance(slots, list) and len(slots) == 2, (
            f"slots.json[{board!r}] must be a list of 2 slots, got {slots!r}"
        )
        for i, s in enumerate(slots):
            assert "name" in s, f"slots.json[{board!r}][{i}]: missing 'name'"
            assert "build" in s and "kind" in s["build"], (
                f"slots.json[{board!r}][{i}]: missing 'build.kind'"
            )


def test_dry_run_t_beam_uses_correct_offsets():
    out = subprocess.run(
        [sys.executable, str(SCRIPT),
         "--board", "t_beam_supreme",
         "--port", "/dev/null",
         "--dry-run"],
        capture_output=True, text=True, check=True,
    ).stdout
    # t_beam_supreme uses 8 MB layout: factory @ 0x20000, ota_0 @ 0x80000, ota_1 @ 0x3b0000
    for off in ("0x20000", "0x80000", "0x3b0000"):
        assert off in out, f"expected offset {off} missing from t_beam dry-run output:\n{out}"
    assert "8MB" in out  # flash size flag


def test_dry_run_xiao_s3_wio_uses_8mb_layout():
    """The XIAO ESP32S3 + Wio-SX1262 kit shares the 8 MB layout, and its
    net-OTA selector env/offsets differ from the plain build."""
    out = subprocess.run(
        [sys.executable, str(SCRIPT),
         "--board", "xiao_s3_wio",
         "--port", "/dev/null",
         "--dry-run"],
        capture_output=True, text=True, check=True,
    ).stdout
    for off in ("0x20000", "0x80000", "0x3b0000"):
        assert off in out, f"expected offset {off} missing from xiao dry-run output:\n{out}"
    assert "8MB" in out

    out_netota = subprocess.run(
        [sys.executable, str(SCRIPT),
         "--board", "xiao_s3_wio", "--net-ota",
         "--port", "/dev/null",
         "--dry-run"],
        capture_output=True, text=True, check=True,
    ).stdout
    for off in ("0x20000", "0xe0000", "0x400000"):
        assert off in out_netota, (
            f"expected net-OTA offset {off} missing from xiao dry-run output:\n{out_netota}"
        )
    assert "0x3b0000" not in out_netota


def test_slot_names_come_from_slots_json():
    """--only <configured name> must work for every board, e.g. the XIAO's
    'Official'/'MeshCore' pair, without any board-specific code."""
    import json as _json
    cfg = _json.loads((REPO / "slots.json").read_text())
    for board, slots in cfg.items():
        if board.startswith("_"):
            continue
        for slot in slots:
            out = subprocess.run(
                [sys.executable, str(SCRIPT), "--board", board,
                 "--port", "/dev/null",
                 "--only", slot["name"], "--dry-run"],
                capture_output=True, text=True,
            )
            assert out.returncode == 0, (
                f"--only {slot['name']!r} failed for {board}:\n{out.stderr}"
            )


if __name__ == "__main__":
    test_boards_offsets_match_csv()
    test_netota_overrides_match_csv()
    test_dry_run_full_flash_emits_expected_offsets()
    test_dry_run_only_meshtastic_writes_just_one_offset()
    test_only_by_slot_index()
    test_slot_path_overrides_default()
    test_slots_json_round_trip()
    test_dry_run_t_beam_uses_correct_offsets()
    test_dry_run_xiao_s3_wio_uses_8mb_layout()
    test_slot_names_come_from_slots_json()
    print("OK")
