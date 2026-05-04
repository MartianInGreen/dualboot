# tests

Quick checks that catch the regressions worth catching:

- **`test_partitions.py`** parses every `partitions*.csv` in the repo and
  asserts alignment, no overlaps, fits in declared flash size, has the
  required slots, and that each `partitions_app*.csv` agrees offset-and-
  size with its full counterpart on every shared partition.
- **`test_flash_all.py`** confirms the `BOARDS` dict in
  `scripts/flash_all.py` has offsets and slot sizes matching the CSVs,
  and that `--dry-run` emits the right `esptool` command for each
  combination of `--board` and `--only`.

## Run

```sh
# Full suite (pytest):
python3 -m pytest tests/ -v

# Standalone, no dependencies (each file is also runnable):
python3 tests/test_partitions.py
python3 tests/test_flash_all.py
```

The partition validator is also useful as an ad-hoc tool when editing
a CSV: run it first, then build the selector. If it fails, the build
would have produced a silently broken layout.
