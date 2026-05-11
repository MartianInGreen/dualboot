// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mdb-dualboot — single-byte selection persistence, no NVS.
//
// We need to remember "which slot did the user pick last?" — exactly one byte.
// Pulling in the full NVS library (Preferences → libnvs_flash → CRCs, page
// management, mutexes) costs ~15-20 KB for that one byte. Instead we own a
// dedicated 12 KB partition (`nvs_sel`) and write the byte raw via the
// esp_partition_* API, which is already linked because we use it for OTA.
//
// Format: a 4-byte magic + 1-byte slot + 3 padding, written at offset 0
// after a single-sector erase. ~100 k erase cycles per sector — at one
// save per boot, decades of headroom.
#pragma once
#include <stdint.h>

namespace mdb_pref {

uint8_t load(uint8_t fallback);
void    save(uint8_t value);

} // namespace mdb_pref
