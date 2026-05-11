// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mesh-dualboot — selector-side network OTA receiver.
//
// Brings up WiFi (softAP and/or station), listens on a single TCP socket,
// authenticates an incoming firmware bin (HMAC-SHA256 and/or Ed25519),
// streams it into ota_0 or ota_1 via esp_ota_*, and returns. Crucially:
// otadata is NEVER touched. The selector keeps boot ownership; the user
// re-picks via menu post-write. Same logical operation as
// `flash_all.py --only`, just over the network.
//
// Compile-time gated by MDB_NET_OTA. Sub-features (transport, trigger,
// auth) gated by MDB_NET_OTA_AP/_STA/_HEADLESS/_AUTH_HMAC/_AUTH_ED25519.
// All sub-features share one partition layout (factory grown to 1 MB);
// see partitions_netota.csv.
#pragma once

#ifdef MDB_NET_OTA

#include <stdint.h>

// ── Compile-time invariants ─────────────────────────────────────────────
#if !defined(MDB_NET_OTA_AP) && !defined(MDB_NET_OTA_STA)
#error "MDB_NET_OTA requires at least one of MDB_NET_OTA_AP or MDB_NET_OTA_STA"
#endif
#if !defined(MDB_NET_OTA_AUTH_HMAC) && !defined(MDB_NET_OTA_AUTH_ED25519)
#error "MDB_NET_OTA requires at least one of MDB_NET_OTA_AUTH_HMAC or MDB_NET_OTA_AUTH_ED25519"
#endif
#if defined(MDB_NET_OTA_AUTH_ED25519) && !defined(MDB_NET_OTA_PUBKEY_HEX)
#error "MDB_NET_OTA_AUTH_ED25519 requires MDB_NET_OTA_PUBKEY_HEX (64-char hex literal)"
#endif

namespace mdb_net_ota {

enum class Transport {
  AP,   // softAP — selector hosts; client joins (proximate / fleet-pinned PSK)
  STA,  // station — selector joins WiFi from NVS-stored creds (remote)
};

// Wire-format status byte sent by the selector after it has processed the
// incoming bin (or detected a fatal error). One byte before close. Lets a
// client tool report success/failure without having to read the OLED.
//
// Codes are stable — pushers may key user-visible messages off them.
namespace status {
  static constexpr uint8_t OK                = 0x00;
  static constexpr uint8_t BAD_HEADER        = 0x01;  // magic or version mismatch
  static constexpr uint8_t SHORT_HEADER      = 0x02;  // socket EOF before full prefix
  static constexpr uint8_t UNSUPPORTED_AUTH  = 0x03;  // auth_type not compiled in
  static constexpr uint8_t SLOT_MISMATCH     = 0x04;  // header slot != menu-locked slot
  static constexpr uint8_t BAD_SLOT          = 0x05;  // slot byte not 0 or 1
  static constexpr uint8_t PARTITION_MISSING = 0x06;  // ota_X partition not found in table
  static constexpr uint8_t BAD_SIZE          = 0x07;  // payload_len 0 or > partition size
  static constexpr uint8_t SHORT_AUTH        = 0x08;  // socket EOF before auth blob done
  static constexpr uint8_t OTA_BEGIN_FAILED  = 0x09;
  static constexpr uint8_t XFER_ABORTED      = 0x0A;  // socket EOF mid-payload
  static constexpr uint8_t OTA_WRITE_FAILED  = 0x0B;
  static constexpr uint8_t BAD_SIG           = 0x0C;  // auth verification failed
  static constexpr uint8_t OTA_END_FAILED    = 0x0D;  // typically image-format invalid
}

#ifdef MDB_NET_OTA_HEADLESS
// Called from setup() before menu draws. If a headless net-flash request
// is pending in nvs_sel (set by the firmware-side request_net_flash()
// helper), runs the session to completion or timeout, draws status to
// the OLED throughout, and returns true. Client picks the target slot
// in the wire header. nf_pending is cleared before listening so a power
// loss during the session does not loop.
bool maybe_handle_pending();
#endif

// Foreground menu-initiated session. Caller binds the target slot
// (user picked from the submenu); the wire header MUST match
// menu_locked_slot (UI promise must match bytes written). Blocking;
// erases the partition on any failure path; does NOT touch otadata.
// Returns true on a verified successful write.
bool run_session(Transport t, int menu_locked_slot);

}  // namespace mdb_net_ota

#endif  // MDB_NET_OTA
