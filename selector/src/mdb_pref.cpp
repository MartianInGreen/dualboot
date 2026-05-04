// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mdb-dualboot — raw selection-byte storage in the `nvs_sel` partition.
#include "mdb_pref.h"

#include <esp_partition.h>
#include <esp_err.h>
#include <string.h>

namespace mdb_pref {

// Custom magic so we can tell our record apart from blank flash (0xFF) or
// some prior NVS payload that was sitting in this sector.
static const uint8_t MAGIC[4] = { 'M', 'D', 'B', 'S' };

static const esp_partition_t* part() {
  // The partition CSV declares this as type=data subtype=nvs, but
  // esp_partition_* doesn't care — we operate on the raw flash region.
  return esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs_sel");
}

uint8_t load(uint8_t fallback) {
  const esp_partition_t* p = part();
  if (!p) return fallback;
  uint8_t buf[8];
  if (esp_partition_read(p, 0, buf, sizeof(buf)) != ESP_OK) return fallback;
  if (memcmp(buf, MAGIC, sizeof(MAGIC)) != 0) return fallback;
  return buf[4];
}

void save(uint8_t value) {
  const esp_partition_t* p = part();
  if (!p) return;
  // Erase one sector (4 KB) — esp_partition_erase_range requires sector
  // alignment. The partition is 12 KB so this is fine.
  if (esp_partition_erase_range(p, 0, 4096) != ESP_OK) return;
  uint8_t rec[8] = { MAGIC[0], MAGIC[1], MAGIC[2], MAGIC[3], value, 0, 0, 0 };
  esp_partition_write(p, 0, rec, sizeof(rec));
}

} // namespace mdb_pref
