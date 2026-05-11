// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mdb-dualboot — selection-byte storage in the `nvs_sel` partition.
//
// Without MDB_NET_OTA: raw 8-byte record at offset 0 (cheap, no NVS overhead).
// With MDB_NET_OTA: proper NVS format so the partition can also hold the
// net-OTA config keys (nf_pending, nf_ssid, nf_psk, nf_timeout_s) written
// by the firmware-side request_net_flash() helper.
#include "mdb_pref.h"

#ifdef MDB_NET_OTA
#include "net_ota/mdb_nvs_sel.h"
#include <nvs.h>
#else
#include <esp_partition.h>
#include <esp_err.h>
#include <string.h>
#endif

namespace mdb_pref {

#ifdef MDB_NET_OTA

uint8_t load(uint8_t fallback) {
  nvs_handle_t h;
  if (!mdb_nvs_sel::open(&h, NVS_READONLY)) return fallback;
  uint8_t v = fallback;
  nvs_get_u8(h, "sel_idx", &v);  // leaves v unchanged on ESP_ERR_NVS_NOT_FOUND
  nvs_close(h);
  return v;
}

void save(uint8_t value) {
  nvs_handle_t h;
  if (!mdb_nvs_sel::open(&h, NVS_READWRITE)) return;
  nvs_set_u8(h, "sel_idx", value);
  nvs_commit(h);
  nvs_close(h);
}

#else  // !MDB_NET_OTA — legacy raw-byte format

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

#endif  // MDB_NET_OTA

} // namespace mdb_pref
