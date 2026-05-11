// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mdb-dualboot — shared NVS handle helper for the `nvs_sel` partition.
// One source of truth for partition-init + namespace conventions, used by
// mdb_pref (selection byte) and the net_ota module (creds, request flag).
//
// Built only when MDB_NET_OTA is defined. The non-net-OTA selector keeps
// its raw-bytes layout in nvs_sel; mixing raw and NVS on the same
// partition is unsafe, so this header is unreferenced in that build.
#pragma once

#ifdef MDB_NET_OTA

#include <nvs.h>

namespace mdb_nvs_sel {

// Partition label and namespace name. The firmware-side helpers
// (request_net_flash, set_net_flash_wifi) must agree on these.
static constexpr const char* PARTITION = "nvs_sel";
static constexpr const char* NAMESPACE = "mdb";

// Lazily initialize the partition (idempotent; safe to call from both
// mdb_pref and the net_ota orchestrator) and open a handle in the
// `mdb` namespace. Returns false on any error; the caller should treat
// that as "use defaults / abort gracefully."
bool open(nvs_handle_t* out_handle, nvs_open_mode_t mode);

}  // namespace mdb_nvs_sel

#endif  // MDB_NET_OTA
