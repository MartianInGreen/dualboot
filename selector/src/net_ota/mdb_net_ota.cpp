// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin
#ifdef MDB_NET_OTA

#include "mdb_net_ota.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

#include "mdb_auth.h"
#include "mdb_nvs_sel.h"
#include "mdb_wifi.h"
#include "../mdb_oled.h"

#ifndef MDB_OLED_ADDR
#define MDB_OLED_ADDR 0x3C
#endif

#ifndef MDB_SLOT0_NAME
#define MDB_SLOT0_NAME "Slot 0"
#endif
#ifndef MDB_SLOT1_NAME
#define MDB_SLOT1_NAME "Slot 1"
#endif

namespace mdb_net_ota {

// ── Constants ───────────────────────────────────────────────────────────
static constexpr uint16_t TCP_PORT = 5050;
static constexpr uint32_t ACCEPT_TIMEOUT_DEFAULT_MS = 5 * 60 * 1000;  // 5 min
static constexpr uint32_t READ_TIMEOUT_MS = 60 * 1000;                // 60 s
// Absolute ceiling on one client's read+stream phase. The per-recv
// READ_TIMEOUT_MS only catches "stalled longer than 60 s"; a slow-loris
// peer dripping one byte every 59 s would otherwise sustain a session
// indefinitely. 5 min easily covers a 1 MB partition at modest link
// rates (1 MB / 50 kbps ≈ 200 s).
static constexpr uint32_t SESSION_TIMEOUT_MS = 5 * 60 * 1000;
static constexpr size_t   PREFIX_BYTES = 12;
static constexpr size_t   STREAM_CHUNK = 1024;
static constexpr uint8_t  WIRE_MAGIC[4] = { 'M', 'D', 'B', 'O' };
static constexpr uint8_t  WIRE_VERSION = 0x01;
static constexpr const char* HMAC_INFO = "mdb-netota-auth-v1";

static const char* slot_label(int slot) {
  return slot == 0 ? MDB_SLOT0_NAME : MDB_SLOT1_NAME;
}

// ── OLED helpers ────────────────────────────────────────────────────────
// Best-effort: if the OLED isn't there mdb_oled::show() silently fails,
// so callers don't need to know whether it's present.

static void show_session_screen(Transport t, const char* line_a,
                                const char* line_b, int target_slot,
                                int progress_pct) {
  mdb_oled::clear();
  mdb_oled::text(0, 0, t == Transport::AP ? "Net flash AP" : "Net flash STA",
                 1, true);
  if (line_a) mdb_oled::text(0, 12, line_a, 1, true);
  if (line_b) mdb_oled::text(0, 22, line_b, 1, true);

  if (target_slot >= 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), "-> %s", slot_label(target_slot));
    mdb_oled::text(0, 32, buf, 1, true);
  } else {
    mdb_oled::text(0, 32, "client picks slot", 1, true);
  }

  if (progress_pct >= 0) {
    int bar_x = 4, bar_y = 50, bar_w = 120, bar_h = 8;
    mdb_oled::rect(bar_x, bar_y, bar_w, bar_h, true);
    int fill = (progress_pct * (bar_w - 2)) / 100;
    if (fill > 0) mdb_oled::rect_fill(bar_x + 1, bar_y + 1, fill, bar_h - 2, true);
  }
  mdb_oled::show(MDB_OLED_ADDR);
}

static void show_result(const char* line, bool ok) {
  mdb_oled::clear();
  mdb_oled::text(0, 0, ok ? "Net flash OK" : "Net flash FAIL", 1, true);
  if (line) mdb_oled::text(0, 18, line, 1, true);
  mdb_oled::show(MDB_OLED_ADDR);
}

// ── Per-session PSK generator ───────────────────────────────────────────
// 8-char draw from a 47-char alphabet (digits + letters minus look-
// alikes), 8 * log2(47) ≈ 44 bits of entropy. Good enough for the
// short-lived WPA2 session window: an attacker has at most ~5 min of
// online attempts against WPA2's retry rate-limit. Offline cracking
// after a handshake capture is feasible on modern GPUs, but the
// listener tears down at session end so any cracked key only signs
// bytes already written to the device.
static void random_psk(char out[9]) {
  static const char alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ"   // no I, O
                              "abcdefghjkmnpqrstuvwxyz"     // no i, l, o
                              "23456789";                   // no 0, 1
  uint8_t r[8];
  esp_fill_random(r, sizeof(r));
  size_t alpha_len = sizeof(alpha) - 1;
  for (int i = 0; i < 8; i++) out[i] = alpha[r[i] % alpha_len];
  out[8] = 0;
}

// ── HMAC-key derivation ─────────────────────────────────────────────────
// HKDF-style separation between the WiFi PSK (printable string) and the
// HMAC auth key (32 bytes). One displayed/stored secret, two distinct
// uses. derive_hmac_key = HMAC-SHA256(psk, HMAC_INFO).
static bool derive_hmac_key(const char* psk, uint8_t out_key[32]) {
  if (!psk || !*psk) return false;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  return mbedtls_md_hmac(info,
                         (const uint8_t*)psk, strlen(psk),
                         (const uint8_t*)HMAC_INFO, strlen(HMAC_INFO),
                         out_key) == 0;
}

// ── Wire-format read helpers ────────────────────────────────────────────

static bool read_exact(int sock, void* buf, size_t n,
                       uint32_t session_deadline_ms) {
  uint8_t* p = (uint8_t*)buf;
  size_t got = 0;
  while (got < n) {
    uint32_t now = millis();
    if (now >= session_deadline_ms) return false;
    uint32_t deadline_left = session_deadline_ms - now;
    uint32_t to = deadline_left < READ_TIMEOUT_MS ? deadline_left : READ_TIMEOUT_MS;
    int r = mdb_wifi::read_bytes(sock, p + got, n - got, to);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

// ── OTA write: stream payload from socket into ota_X partition ──────────
//
// `prefix` is the 12 wire-prefix bytes (already validated). `auth_blob` is
// the auth bytes that follow the prefix. Streams `payload_len` bytes from
// the socket into the partition while updating a running SHA-256 over
// (prefix || payload), then verifies the auth blob against that hash.
// On any failure after esp_ota_begin (which erases the partition), the
// partition is force-erased so a half-write doesn't pretend to be valid.
//
// Returns status::OK on success, or one of the streaming-related error
// codes. Caller is responsible for sending the byte to the client and
// closing the socket — keeps socket lifecycle in one place.
static uint8_t stream_to_partition(int sock,
                                   const esp_partition_t* part,
                                   const uint8_t prefix[PREFIX_BYTES],
                                   uint32_t payload_len,
                                   uint8_t auth_type,
                                   const uint8_t* auth_blob,
                                   size_t auth_blob_len,
                                   const uint8_t* hmac_key,
                                   size_t hmac_key_len,
                                   int progress_target_slot,
                                   Transport t,
                                   uint32_t session_deadline_ms) {
  esp_ota_handle_t h = 0;
  esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h);  // erases partition
  if (err != ESP_OK) {
    show_result(esp_err_to_name(err), false);
    return status::OTA_BEGIN_FAILED;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);  // is224 = 0 → SHA-256, not SHA-224
  mbedtls_sha256_update(&sha, prefix, PREFIX_BYTES);

  uint8_t buf[STREAM_CHUNK];
  uint32_t remaining = payload_len;
  uint32_t total = payload_len;
  int last_pct_drawn = -1;

  while (remaining > 0) {
    uint32_t now = millis();
    if (now >= session_deadline_ms) {
      esp_ota_abort(h);
      esp_partition_erase_range(part, 0, part->size);
      mbedtls_sha256_free(&sha);
      show_result("session timeout", false);
      return status::XFER_ABORTED;
    }
    uint32_t deadline_left = session_deadline_ms - now;
    uint32_t to = deadline_left < READ_TIMEOUT_MS ? deadline_left : READ_TIMEOUT_MS;
    size_t want = remaining < STREAM_CHUNK ? remaining : STREAM_CHUNK;
    int got = mdb_wifi::read_bytes(sock, buf, want, to);
    if (got <= 0) {
      esp_ota_abort(h);
      esp_partition_erase_range(part, 0, part->size);
      mbedtls_sha256_free(&sha);
      show_result("xfer aborted", false);
      return status::XFER_ABORTED;
    }
    if (esp_ota_write(h, buf, got) != ESP_OK) {
      esp_ota_abort(h);
      esp_partition_erase_range(part, 0, part->size);
      mbedtls_sha256_free(&sha);
      show_result("ota_write failed", false);
      return status::OTA_WRITE_FAILED;
    }
    mbedtls_sha256_update(&sha, buf, got);
    remaining -= (uint32_t)got;

    int pct = (int)(((uint64_t)(total - remaining) * 100) / (total ? total : 1));
    if (pct != last_pct_drawn) {
      last_pct_drawn = pct;
      char line[32];
      snprintf(line, sizeof(line), "%lu / %lu KB",
               (unsigned long)((total - remaining) / 1024),
               (unsigned long)(total / 1024));
      show_session_screen(t, "transferring...", line,
                          progress_target_slot, pct);
    }
  }

  uint8_t hash[32];
  mbedtls_sha256_finish(&sha, hash);
  mbedtls_sha256_free(&sha);

  if (!mdb_auth::verify(auth_type, hash, auth_blob, auth_blob_len,
                        hmac_key, hmac_key_len)) {
    esp_ota_abort(h);
    esp_partition_erase_range(part, 0, part->size);
    show_result("bad signature", false);
    return status::BAD_SIG;
  }

  err = esp_ota_end(h);
  if (err != ESP_OK) {
    // esp_ota_end can fail if the underlying image isn't a valid ESP
    // image (e.g. wrong chip, corrupted bytes). Erase to be safe.
    esp_partition_erase_range(part, 0, part->size);
    show_result(esp_err_to_name(err), false);
    return status::OTA_END_FAILED;
  }

  return status::OK;
}

// Send a status byte and close the socket, then surface the message on
// the OLED. Used for every fail-fast path in handle_one_session so the
// pusher tool can map the status byte to a human-readable cause.
//
// Returns 0 ("a client was processed, success or failure") so callers
// can `return fail_one(...)` directly from handle_one_session's int
// return type.
static int fail_one(int sock, uint8_t st, const char* msg) {
  mdb_wifi::write_byte(sock, st);
  mdb_wifi::close_socket(sock);
  show_result(msg, false);
  return 0;
}

// ── One client session ──────────────────────────────────────────────────
//
// Accepts one TCP client, validates, streams the bin to the chosen slot,
// sends a status byte, closes the socket. Returns:
//   -1 → accept timed out (no client connected within accept_timeout_ms;
//        caller should stop looping and tear down WiFi)
//    0 → a client connected and was processed (success or failure; OLED
//        and the wire status byte have already reported the outcome)
//
// This split (vs. returning a single bool) is what lets the headless
// path loop accept_one until the deadline hits, so a single trigger can
// flash both slots back-to-back without re-triggering. Menu mode calls
// this once and is done.
//
// menu_locked_slot: -1 means client picks (headless); 0/1 means caller-bound.
// hmac_key/hmac_key_len: HKDF-derived HMAC key, or null if HMAC not used.

static int handle_one_session(int menu_locked_slot,
                              const uint8_t* hmac_key, size_t hmac_key_len,
                              uint32_t accept_timeout_ms,
                              Transport t,
                              const char* status_line,
                              const char* psk_for_display) {
  // Build the two body lines for the OLED:
  //   AP mode  → line_a = "PSK <random>"           (operator types this)
  //              line_b = status_line (IP:port)    (operator connects here)
  //   STA mode → line_a = status_line (host.local:port)  (primary target)
  //              line_b = ip:port                  (fallback if mDNS blocked)
  // The waiting state used to say "PSK shown below" / "waiting..." which
  // omitted the IP in AP mode and the fallback IP in STA mode — both gaps
  // an operator hits in the field.
  char line_a_buf[32];
  char line_b_buf[32];
  const char* line_a;
  const char* line_b;
  if (psk_for_display) {
    snprintf(line_a_buf, sizeof(line_a_buf), "PSK %s", psk_for_display);
    line_a = line_a_buf;
    line_b = status_line;
  } else {
    line_a = status_line;
    char ip[20];
    mdb_wifi::current_ip(ip, sizeof(ip));
    snprintf(line_b_buf, sizeof(line_b_buf), "%s:%u", ip, (unsigned)TCP_PORT);
    line_b = line_b_buf;
  }
  show_session_screen(t, line_a, line_b, menu_locked_slot, -1);

  int sock = mdb_wifi::accept_one(accept_timeout_ms);
  if (sock < 0) {
    // No client within the timeout. Caller decides whether to retry or
    // tear down — don't show "accept timeout" here because in the
    // headless loop case it's the normal exit condition.
    return -1;
  }

  // Absolute deadline for everything this client gets to do (prefix read,
  // auth read, payload stream). Enforces an upper bound that the per-recv
  // READ_TIMEOUT_MS alone can't — see SESSION_TIMEOUT_MS comment above.
  uint32_t session_deadline_ms = millis() + SESSION_TIMEOUT_MS;

  // Read prefix.
  uint8_t prefix[PREFIX_BYTES];
  if (!read_exact(sock, prefix, sizeof(prefix), session_deadline_ms)) {
    return fail_one(sock, status::SHORT_HEADER, "short header");
  }

  // Validate magic + version.
  if (memcmp(prefix, WIRE_MAGIC, 4) != 0 || prefix[4] != WIRE_VERSION) {
    return fail_one(sock, status::BAD_HEADER, "bad magic/ver");
  }
  // Reserved byte must be zero. Lock it down so future protocol
  // extensions can't quietly repurpose it on devices running an older
  // selector — any change must bump WIRE_VERSION instead. The byte is
  // already in the signed prefix, but signing alone doesn't constrain
  // semantics for the device.
  if (prefix[7] != 0) {
    return fail_one(sock, status::BAD_HEADER, "bad reserved");
  }

  uint8_t auth_type = prefix[5];
  uint8_t target_slot = prefix[6];
  uint32_t payload_len = ((uint32_t)prefix[8] << 24) |
                         ((uint32_t)prefix[9] << 16) |
                         ((uint32_t)prefix[10] << 8) |
                         ((uint32_t)prefix[11]);

  // Validate auth type compiled in.
  size_t auth_len = mdb_auth::blob_size(auth_type);
  if (auth_len == 0) {
    return fail_one(sock, status::UNSUPPORTED_AUTH, "unsupported auth");
  }

  // Hybrid target binding.
  if (menu_locked_slot >= 0 && (int)target_slot != menu_locked_slot) {
    return fail_one(sock, status::SLOT_MISMATCH, "slot mismatch");
  }
  if (target_slot != 0 && target_slot != 1) {
    return fail_one(sock, status::BAD_SLOT, "bad slot");
  }

  // Locate target partition; check size bound before erasing anything.
  esp_partition_subtype_t sub = target_slot == 0
      ? ESP_PARTITION_SUBTYPE_APP_OTA_0
      : ESP_PARTITION_SUBTYPE_APP_OTA_1;
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, sub, nullptr);
  if (!part) {
    return fail_one(sock, status::PARTITION_MISSING, "partition?");
  }
  if (payload_len == 0 || payload_len > part->size) {
    return fail_one(sock, status::BAD_SIZE, "size bound");
  }

  // Read auth blob.
  uint8_t auth_blob[64];
  if (auth_len > sizeof(auth_blob) ||
      !read_exact(sock, auth_blob, auth_len, session_deadline_ms)) {
    return fail_one(sock, status::SHORT_AUTH, "short auth");
  }

  // Stream → write → verify.
  uint8_t st = stream_to_partition(sock, part, prefix, payload_len,
                                    auth_type, auth_blob, auth_len,
                                    hmac_key, hmac_key_len,
                                    (int)target_slot, t,
                                    session_deadline_ms);
  // Always send the status byte (success or failure code) before close —
  // this is what lets mdb_push.py report the outcome to the operator.
  mdb_wifi::write_byte(sock, st);
  mdb_wifi::close_socket(sock);
  if (st == status::OK) {
    char done[32];
    snprintf(done, sizeof(done), "%s OK", slot_label(target_slot));
    show_result(done, true);
    delay(1500);
  }
  return 0;
}

// ── Public: foreground menu-initiated session ───────────────────────────

bool run_session(Transport t, int menu_locked_slot) {
  if (menu_locked_slot != 0 && menu_locked_slot != 1) return false;

  char psk[9];
  random_psk(psk);

  uint8_t hmac_key[32];
  bool have_hmac = derive_hmac_key(psk, hmac_key);

  char ssid[24];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(ssid, sizeof(ssid), "mdb-%02x%02x", mac[4], mac[5]);

  char status[24];
  if (!mdb_wifi::start(t, ssid, psk, TCP_PORT, status, sizeof(status))) {
    show_result("wifi start failed", false);
    delay(2000);
    return false;
  }

  // Menu-initiated path: single-shot. Operator picked a slot and is
  // watching the OLED; one accept attempt is enough. (Headless mode
  // loops until deadline — see maybe_handle_pending below.)
  int r = handle_one_session(menu_locked_slot,
                             have_hmac ? hmac_key : nullptr,
                             have_hmac ? sizeof(hmac_key) : 0,
                             ACCEPT_TIMEOUT_DEFAULT_MS,
                             t,
                             status,
                             t == Transport::AP ? psk : nullptr);
  if (r < 0) {
    show_result("accept timeout", false);
    delay(1500);
  }
  mdb_wifi::stop();
  return r == 0;  // "we processed a client" — caller treats as session done
}

// ── Public: headless trigger handler ────────────────────────────────────

#ifdef MDB_NET_OTA_HEADLESS

bool maybe_handle_pending() {
  nvs_handle_t h;
  if (!mdb_nvs_sel::open(&h, NVS_READWRITE)) return false;

  uint8_t pending = 0;
  if (nvs_get_u8(h, "nf_pending", &pending) != ESP_OK || !pending) {
    nvs_close(h);
    return false;
  }

  // Default to 5 min; firmware-side helper can override. A firmware bug
  // that writes 0 (or a huge value) shouldn't brick the user out of the
  // listener — clamp on read so misconfiguration is graceful.
  uint16_t timeout_s = 0;
  nvs_get_u16(h, "nf_timeout_s", &timeout_s);
  if (timeout_s == 0 || timeout_s > 3600) timeout_s = 300;

  char ssid[33] = {0};
  char psk[65] = {0};
  size_t ssid_len = sizeof(ssid);
  size_t psk_len = sizeof(psk);
  esp_err_t e_ssid = nvs_get_str(h, "nf_ssid", ssid, &ssid_len);
  esp_err_t e_psk  = nvs_get_str(h, "nf_psk",  psk,  &psk_len);

  // One-shot consumption: clear the pending flag AND scrub the creds in
  // one commit, BEFORE running the session. Up-front (vs. post-session)
  // means a mid-session crash or power loss leaves NVS already clean,
  // minimizing the window during which a physical-flash attacker could
  // recover the operator's WiFi password. NVS doesn't securely scrub
  // overwritten values — stale copies persist in the page log until GC —
  // so "never store longer than necessary" is the only real mitigation.
  // Future triggers must re-pass creds via request_net_flash() (which the
  // API signature requires anyway). This also subsumes the prior
  // anti-loop-on-crash guarantee: if we crash before the commit lands,
  // nf_pending stays set and the next boot retries — identical to the
  // old behavior at that boundary.
  nvs_set_u8(h, "nf_pending", 0);
  nvs_erase_key(h, "nf_ssid");  // NOT_FOUND tolerated
  nvs_erase_key(h, "nf_psk");
  nvs_commit(h);
  nvs_close(h);

  // Pick transport: STA if creds are present, AP otherwise (which uses
  // the same PSK as both the AP password and the HMAC seed; an AP
  // session without creds in NVS would have nothing to derive from, so
  // require psk in either case).
  if (e_psk != ESP_OK || psk[0] == 0) {
    show_result("no PSK in NVS", false);
    delay(2000);
    return true;  // we did "handle" the pending flag, even if the session can't run
  }

  // Transport selection across the four AP/STA flag combinations. The
  // dangling-`else` chain below is deliberate; the preprocessor stitches
  // it into a sensible expression in each compiled variant:
  //   AP + STA: try STA when creds are set, else fall through to AP.
  //   STA only: try STA when creds are set; otherwise the AP `{...}`
  //             block vanishes and the `else` attaches to the #else
  //             "no transport" branch.
  //   AP only:  the entire STA `if (...) else` vanishes, leaving an
  //             unconditional AP block as the body.
  //   neither:  blocked at compile time by the #error invariants in
  //             mdb_net_ota.h, so this code is never reached.
  Transport t;
#ifdef MDB_NET_OTA_STA
  if (e_ssid == ESP_OK && ssid[0] != 0) {
    t = Transport::STA;
  } else
#endif
#ifdef MDB_NET_OTA_AP
  {
    t = Transport::AP;
    // Use a stable per-device SSID for the AP.
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(ssid, sizeof(ssid), "mdb-%02x%02x", mac[4], mac[5]);
  }
#else
  {
    show_result("no transport", false);
    delay(2000);
    return true;
  }
#endif

  uint8_t hmac_key[32];
  bool have_hmac = derive_hmac_key(psk, hmac_key);

  char status[24];
  if (!mdb_wifi::start(t, ssid, psk, TCP_PORT, status, sizeof(status))) {
    show_result("wifi start failed", false);
    delay(2000);
    return true;
  }

  // Headless mode: loop accept until the global deadline. Each successful
  // (or failed) push consumes one accept; the listener stays up until
  // either the operator's deadline expires or no client connects within
  // a single accept window. This is what makes "single trigger flashes
  // both slots" actually work — the client picks the target slot in the
  // wire header, so back-to-back pushes hit different slots.
  uint32_t deadline = millis() + (uint32_t)timeout_s * 1000;
  while (true) {
    uint32_t now = millis();
    if (now >= deadline) break;
    uint32_t remaining = deadline - now;
    int r = handle_one_session(/*menu_locked_slot=*/-1,
                               have_hmac ? hmac_key : nullptr,
                               have_hmac ? sizeof(hmac_key) : 0,
                               remaining,
                               t, status,
                               /*psk_for_display=*/nullptr);
    if (r < 0) break;  // accept timed out — no more clients within window
  }
  mdb_wifi::stop();
  return true;
}

#endif  // MDB_NET_OTA_HEADLESS

}  // namespace mdb_net_ota

#endif  // MDB_NET_OTA
