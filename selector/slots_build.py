# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 The Official Mesh Admin

"""PlatformIO pre-script: inject slot names from slots.json into build flags.

Reads ../slots.json, looks up the active env name (which equals the board key),
and appends -D MDB_SLOT0_NAME / -D MDB_SLOT1_NAME so main.cpp can print the
configured labels on the OLED.
"""
import json
import os

Import("env")  # type: ignore[name-defined]  # noqa: F821 — provided by PlatformIO


def _c_string(s: str) -> str:
    # Quote for the C preprocessor, escaping backslashes and quotes.
    return '\\"' + s.replace("\\", "\\\\").replace('"', '\\"') + '\\"'


def _load():
    project_dir = env["PROJECT_DIR"]  # type: ignore[name-defined]  # noqa: F821
    path = os.path.join(project_dir, "..", "slots.json")
    with open(path, "r") as f:
        return json.load(f)


def _apply():
    cfg = _load()
    pioenv = env["PIOENV"]  # type: ignore[name-defined]  # noqa: F821
    slots = cfg.get(pioenv)
    if not slots:
        print(f"[slots_build] no entry for env {pioenv!r} in slots.json — using defaults")
        return
    if len(slots) != 2:
        raise SystemExit(f"slots.json[{pioenv!r}]: expected exactly 2 slots, got {len(slots)}")
    s0 = slots[0]["name"]
    s1 = slots[1]["name"]
    env.Append(BUILD_FLAGS=[  # type: ignore[name-defined]  # noqa: F821
        f"-D MDB_SLOT0_NAME={_c_string(s0)}",
        f"-D MDB_SLOT1_NAME={_c_string(s1)}",
    ])
    print(f"[slots_build] {pioenv}: slot0={s0!r} slot1={s1!r}")


_apply()
