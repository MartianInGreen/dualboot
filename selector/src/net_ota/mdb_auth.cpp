// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin
#ifdef MDB_NET_OTA

#include "mdb_auth.h"

#include <string.h>

#ifdef MDB_NET_OTA_AUTH_HMAC
#include <mbedtls/md.h>
#endif

#ifdef MDB_NET_OTA_AUTH_ED25519
#include <ed25519.h>  // selector/lib/ed25519/src/ — orlp/ed25519, Zlib

#ifndef MDB_NET_OTA_PUBKEY_HEX
#error "MDB_NET_OTA_AUTH_ED25519 requires MDB_NET_OTA_PUBKEY_HEX"
#endif
#endif

namespace mdb_auth {

#ifdef MDB_NET_OTA_AUTH_HMAC
static bool verify_hmac(const uint8_t* hash32,
                        const uint8_t* blob, size_t blob_len,
                        const uint8_t* key, size_t key_len) {
  if (blob_len != 32 || key_len == 0) return false;
  uint8_t expected[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  if (mbedtls_md_hmac(info, key, key_len, hash32, 32, expected) != 0) return false;
  // Constant-time compare to avoid timing side-channels on the auth check.
  uint8_t diff = 0;
  for (size_t i = 0; i < 32; i++) diff |= (expected[i] ^ blob[i]);
  return diff == 0;
}
#endif

#ifdef MDB_NET_OTA_AUTH_ED25519
// Parse MDB_NET_OTA_PUBKEY_HEX (a 64-char hex string literal) once into a
// 32-byte pubkey. Static cache; first call populates, subsequent calls reuse.
static bool load_pubkey(uint8_t out[32]) {
  static uint8_t cached[32];
  static bool ready = false;
  if (ready) { memcpy(out, cached, 32); return true; }

  const char* hex = MDB_NET_OTA_PUBKEY_HEX;
  if (strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    int hi = hex[i*2], lo = hex[i*2 + 1];
    auto nibble = [](int c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hv = nibble(hi), lv = nibble(lo);
    if (hv < 0 || lv < 0) return false;
    cached[i] = (uint8_t)((hv << 4) | lv);
  }
  ready = true;
  memcpy(out, cached, 32);
  return true;
}

static bool verify_ed25519(const uint8_t* hash32,
                           const uint8_t* sig, size_t sig_len) {
  if (sig_len != 64) return false;
  uint8_t pubkey[32];
  if (!load_pubkey(pubkey)) return false;
  // ed25519_verify returns 1 on valid, 0 on invalid. Signing tool must
  // sign the 32-byte sha256(prefix||payload) directly (Ed25519 over the
  // hash, not the bin — matches the selector's streaming-friendly design).
  return ed25519_verify(sig, hash32, 32, pubkey) == 1;
}
#endif

size_t blob_size(uint8_t auth_type) {
  switch (auth_type) {
#ifdef MDB_NET_OTA_AUTH_HMAC
    case TYPE_HMAC_SHA256: return 32;
#endif
#ifdef MDB_NET_OTA_AUTH_ED25519
    case TYPE_ED25519: return 64;
#endif
    default: return 0;
  }
}

bool verify(uint8_t auth_type,
            const uint8_t* hash32,
            const uint8_t* auth_blob, size_t auth_blob_len,
            const uint8_t* hmac_key, size_t hmac_key_len) {
  switch (auth_type) {
#ifdef MDB_NET_OTA_AUTH_HMAC
    case TYPE_HMAC_SHA256:
      return verify_hmac(hash32, auth_blob, auth_blob_len, hmac_key, hmac_key_len);
#endif
#ifdef MDB_NET_OTA_AUTH_ED25519
    case TYPE_ED25519:
      (void)hmac_key; (void)hmac_key_len;
      return verify_ed25519(hash32, auth_blob, auth_blob_len);
#endif
    default:
      return false;
  }
}

}  // namespace mdb_auth

#endif  // MDB_NET_OTA
