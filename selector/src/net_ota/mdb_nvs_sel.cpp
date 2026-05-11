// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin
#ifdef MDB_NET_OTA

#include "mdb_nvs_sel.h"

#include <nvs_flash.h>

namespace mdb_nvs_sel {

static bool inited = false;

static bool ensure_inited() {
  if (inited) return true;
  esp_err_t err = nvs_flash_init_partition(PARTITION);
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // Partition is in a state NVS can't use as-is — typically because it
    // was previously raw-formatted (legacy mdb_pref) or carries data from
    // an older NVS version. Erase and re-init.
    if (nvs_flash_erase_partition(PARTITION) != ESP_OK) return false;
    err = nvs_flash_init_partition(PARTITION);
  }
  if (err != ESP_OK) return false;
  inited = true;
  return true;
}

bool open(nvs_handle_t* out_handle, nvs_open_mode_t mode) {
  if (!ensure_inited()) return false;
  return nvs_open_from_partition(PARTITION, NAMESPACE, mode, out_handle) == ESP_OK;
}

}  // namespace mdb_nvs_sel

#endif  // MDB_NET_OTA
