// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin

// mesh-dualboot net-OTA — WiFi + TCP listener helpers.
//
// One unified interface for both transports. AP brings up a WPA2 softAP
// and listens on 192.168.4.1; STA joins an existing network and listens
// on the resolved DHCP IP. Both go through the same accept_one() →
// read_bytes() → close_socket() flow.
#pragma once

#ifdef MDB_NET_OTA

#include "mdb_net_ota.h"
#include <stddef.h>
#include <stdint.h>

namespace mdb_wifi {

// Bring up the requested transport. ssid/psk are required for AP (used
// as the AP's own SSID/password) and STA (the network being joined).
// Writes a one-line "primary target" status string into out_status for
// OLED display:
//   AP mode  → "192.168.4.1:5050"      (the only useful target)
//   STA mode → "mdb-XXYY.local:5050"   (mDNS hostname; IP is fallback)
// Returns true on success.
bool start(mdb_net_ota::Transport t, const char* ssid, const char* psk,
           uint16_t port, char* out_status, size_t out_status_size);

// After start() returned true, writes the dotted-quad IP of the active
// interface into `out`. AP mode → "192.168.4.1". STA mode → DHCP-assigned
// IP. Used for OLED display alongside the primary target so the operator
// sees both the mDNS hostname and the raw IP (in case multicast is
// blocked on their LAN).
void current_ip(char* out, size_t out_size);

// Stop and tear down WiFi.
void stop();

// Block on accept() up to timeout_ms. Returns the connected socket fd
// or -1 on timeout/error.
int accept_one(uint32_t timeout_ms);

// Read up to n bytes from the accepted socket; blocks up to timeout_ms.
// Returns bytes actually read (>=0), or -1 on error/disconnect.
int read_bytes(int sock, void* buf, size_t n, uint32_t timeout_ms);

// Best-effort single-byte send (used for the protocol's status response
// before close). Returns true if the byte was queued for transmission.
bool write_byte(int sock, uint8_t b);

void close_socket(int sock);

}  // namespace mdb_wifi

#endif  // MDB_NET_OTA
