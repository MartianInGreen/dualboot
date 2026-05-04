# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 The Official Mesh Admin

"""Parser + validator for the project's partitions*.csv files.

A partition CSV (ESP-IDF format) is a comma-separated table of:
    Name, Type, SubType, Offset, Size [, Flags]

Lines starting with '#' or blank lines are ignored. Numeric fields may be
decimal or 0x-prefixed hex.

Used by the test suite and also runnable as a standalone CLI for ad-hoc
checks during partition edits.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

ALIGN_DATA = 0x1000          # 4 KB
ALIGN_APP  = 0x10000         # 64 KB

REQUIRED_TYPES = {"otadata", "ota_0", "ota_1"}  # subtypes really, see Partition.subtype
APP_SUBTYPES   = {"factory", "ota_0", "ota_1"}


@dataclass(frozen=True)
class Partition:
    name: str
    type: str
    subtype: str
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size

    @property
    def is_app(self) -> bool:
        return self.type == "app"


def _parse_int(field: str) -> int:
    field = field.strip()
    return int(field, 16) if field.lower().startswith("0x") else int(field)


def parse(path: Path) -> list[Partition]:
    parts: list[Partition] = []
    for raw in Path(path).read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cells = [c.strip() for c in line.split(",")]
        cells = [c for c in cells if c != ""]
        if len(cells) < 5:
            raise ValueError(f"{path}: bad row {raw!r}")
        parts.append(Partition(
            name=cells[0],
            type=cells[1],
            subtype=cells[2],
            offset=_parse_int(cells[3]),
            size=_parse_int(cells[4]),
        ))
    return parts


def validate(parts: Iterable[Partition], flash_size: int, *, allow_factory_missing: bool) -> list[str]:
    """Return a list of error messages; empty list = OK."""
    parts = list(parts)
    errors: list[str] = []

    # alignment
    for p in parts:
        align = ALIGN_APP if p.is_app else ALIGN_DATA
        if p.offset % align != 0:
            errors.append(f"{p.name}: offset 0x{p.offset:x} not aligned to 0x{align:x}")
        if p.size % ALIGN_DATA != 0:
            errors.append(f"{p.name}: size 0x{p.size:x} not 4 KB aligned")
        if p.size == 0:
            errors.append(f"{p.name}: zero size")

    # overlap
    for a, b in zip(sorted(parts, key=lambda x: x.offset),
                    sorted(parts, key=lambda x: x.offset)[1:]):
        if a.end > b.offset:
            errors.append(f"overlap: {a.name} ends 0x{a.end:x}, {b.name} starts 0x{b.offset:x}")

    # bounds
    for p in parts:
        if p.end > flash_size:
            errors.append(f"{p.name}: extends past flash end (0x{p.end:x} > 0x{flash_size:x})")

    # required slots
    subs = {p.subtype for p in parts}
    needed = {"ota", "ota_0", "ota_1"}  # otadata's subtype is "ota"
    if not allow_factory_missing:
        needed.add("factory")
    missing = needed - subs
    if missing:
        errors.append(f"missing required subtype(s): {sorted(missing)}")

    return errors


def diff_pair(full_parts: list[Partition], app_parts: list[Partition]) -> list[str]:
    """Verify the only structural difference between a full + app CSV is the
    presence of the `factory` partition in the full table."""
    errors: list[str] = []
    full_by_name = {p.name: p for p in full_parts}
    app_by_name  = {p.name: p for p in app_parts}

    if "factory" in app_by_name:
        errors.append("app CSV unexpectedly contains 'factory'")
    if "factory" not in full_by_name:
        errors.append("full CSV is missing 'factory'")

    extra_in_full = (set(full_by_name) - set(app_by_name)) - {"factory"}
    extra_in_app  = set(app_by_name) - set(full_by_name)
    if extra_in_full:
        errors.append(f"partitions only in full CSV (besides factory): {sorted(extra_in_full)}")
    if extra_in_app:
        errors.append(f"partitions only in app CSV: {sorted(extra_in_app)}")

    for name, ap in app_by_name.items():
        fp = full_by_name.get(name)
        if fp and (fp.offset != ap.offset or fp.size != ap.size):
            errors.append(
                f"{name}: drift — full=(0x{fp.offset:x},0x{fp.size:x}) "
                f"app=(0x{ap.offset:x},0x{ap.size:x})"
            )
    return errors
