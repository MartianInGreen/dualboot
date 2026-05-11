// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mesh-dualboot net-OTA — auth blob verification.
//
// Wire format declares auth_type as a single byte:
//   0x01 = HMAC-SHA256(key, hash) — 32 bytes
//   0x02 = Ed25519 sig over hash  — 64 bytes (build-time-baked pubkey)
//
// Both auth modes verify the SAME 32-byte hash: sha256(prefix12 || payload).
// The selector accumulates this hash while streaming payload to flash
// and only commits (esp_ota_end) after the auth blob verifies. Ed25519
// verify uses orlp/ed25519 (selector/lib/ed25519, Zlib license, git
// submodule pinned via .gitmodules).
#pragma once

#ifdef MDB_NET_OTA

#include <stddef.h>
#include <stdint.h>

namespace mdb_auth {

constexpr uint8_t TYPE_HMAC_SHA256 = 0x01;
constexpr uint8_t TYPE_ED25519 = 0x02;

// Returns the on-wire size of the auth blob for this type, or 0 if the
// type is not compiled in. Caller uses this to know how many bytes to
// read from the socket after the 12-byte prefix.
size_t blob_size(uint8_t auth_type);

// Verify auth_blob against `hash32` (a SHA-256 of prefix||payload).
// For HMAC: hmac_key is the per-session/NVS PSK (HKDF-derived).
// For Ed25519: hmac_key is unused (uses the build-time-baked pubkey).
// Returns true iff the signature/MAC checks out.
bool verify(uint8_t auth_type,
            const uint8_t* hash32,
            const uint8_t* auth_blob, size_t auth_blob_len,
            const uint8_t* hmac_key, size_t hmac_key_len);

}  // namespace mdb_auth

#endif  // MDB_NET_OTA
