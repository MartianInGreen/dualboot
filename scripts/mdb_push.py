#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 The Official Mesh Admin

"""mesh-dualboot net-OTA pusher.

Connects to a selector running its WiFi+TCP listener, sends a
signed firmware bin (output from mdb_sign.py), reports the result.

Pre-flight:
  1. Sign the bin with mdb_sign.py.
  2. (AP mode) Join the device's "mdb-XXYY" softAP using the PSK shown
     on its OLED. The selector prints a status line like 192.168.4.1:5050.
     (STA mode) The selector joins your existing network. Find its IP via:
       - the OLED status line, or
       - mDNS hostname (`mdb-XXYY.local`) if the selector was built with
         the mDNS announce on, or
       - your router's DHCP table.
  3. Run this script with the host + signed bin.

Status byte mapping (matches mdb_net_ota.h::status::*):
"""

import argparse
import socket
import sys
import time

# Status codes — keep in sync with selector/src/net_ota/mdb_net_ota.h
STATUS_NAMES = {
    0x00: "OK",
    0x01: "BAD_HEADER (magic/version mismatch)",
    0x02: "SHORT_HEADER (socket EOF before full prefix)",
    0x03: "UNSUPPORTED_AUTH (auth_type not built into this selector)",
    0x04: "SLOT_MISMATCH (header slot != menu-locked slot)",
    0x05: "BAD_SLOT (slot byte not 0 or 1)",
    0x06: "PARTITION_MISSING (ota_X partition not found)",
    0x07: "BAD_SIZE (payload_len 0 or > partition size)",
    0x08: "SHORT_AUTH (socket EOF before auth blob done)",
    0x09: "OTA_BEGIN_FAILED",
    0x0A: "XFER_ABORTED (socket EOF mid-payload)",
    0x0B: "OTA_WRITE_FAILED",
    0x0C: "BAD_SIG (signature/HMAC verification failed)",
    0x0D: "OTA_END_FAILED (typically image-format invalid)",
}


def push(host: str, port: int, bin_path: str,
         connect_timeout_s: float, response_timeout_s: float,
         chunk_size: int) -> int:
    with open(bin_path, "rb") as f:
        data = f.read()
    if not data:
        sys.exit(f"{bin_path} is empty")

    print(f"[mdb_push] connecting to {host}:{port} ({len(data):,} bytes)")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(connect_timeout_s)
    s.connect((host, port))
    # Use a generous socket timeout for the streaming send; the device's
    # esp_ota_begin erases the partition (~70 ms/sector × 1280 sectors for
    # the 5 MB slot ≈ 90 s worst case) before it'll consume the next byte
    # off the wire. lwIP's TCP buffers absorb the rest.
    s.settimeout(max(connect_timeout_s, 30.0))

    sent = 0
    t0 = time.monotonic()
    next_log = 0
    while sent < len(data):
        n = s.send(data[sent:sent + chunk_size])
        if n <= 0:
            sys.exit(f"[mdb_push] send failed at offset {sent}")
        sent += n
        if sent >= next_log or sent == len(data):
            pct = sent * 100 // len(data)
            elapsed = time.monotonic() - t0
            rate_kbs = (sent / 1024) / elapsed if elapsed > 0 else 0
            print(f"[mdb_push]   {sent:>9,}/{len(data):,} ({pct:>3}%)  "
                  f"{elapsed:>5.1f}s  {rate_kbs:>5.1f} KB/s")
            next_log = sent + 256 * 1024

    print(f"[mdb_push] payload sent — waiting for status byte "
          f"(up to {response_timeout_s:.0f}s for OTA write + verify)...")
    s.settimeout(response_timeout_s)
    try:
        resp = s.recv(1)
    except socket.timeout:
        print("[mdb_push] timeout waiting for response — device may have crashed mid-write.")
        s.close()
        return 2

    s.close()
    if not resp:
        print("[mdb_push] device closed without status byte "
              "(older selector, or transport error).")
        return 2
    code = resp[0]
    label = STATUS_NAMES.get(code, f"unknown 0x{code:02x}")
    if code == 0:
        print(f"[mdb_push] device reports: OK ({label})")
        return 0
    print(f"[mdb_push] device reports error 0x{code:02x}: {label}")
    return 1


def main():
    ap = argparse.ArgumentParser(
        description="mesh-dualboot net-OTA pusher",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Pre-flight:\n"
            "  1. mdb_sign.py sign --hmac --psk ABCDEFGH --slot 1 --in fw.bin --out fw.signed\n"
            "  2. Join the device's AP (or be on the same LAN in STA mode).\n"
            "  3. mdb_push.py --host 192.168.4.1 --bin fw.signed\n"
        ),
    )
    ap.add_argument("--host", required=True,
                    help="device IP, hostname, or mdb-XXYY.local")
    ap.add_argument("--port", type=int, default=5050)
    ap.add_argument("--bin", required=True,
                    help="signed firmware bin (output from mdb_sign.py sign)")
    ap.add_argument("--connect-timeout", type=float, default=10.0)
    ap.add_argument("--response-timeout", type=float, default=30.0,
                    help="how long to wait after sending for the device's "
                         "status byte (covers OTA erase + write + verify). "
                         "Default 30s reflects a typical bench / fast-LAN "
                         "session; raise it (e.g. 120) for very large "
                         "firmware bins or congested links")
    ap.add_argument("--chunk-size", type=int, default=4096)
    args = ap.parse_args()

    sys.exit(push(args.host, args.port, args.bin,
                  args.connect_timeout, args.response_timeout,
                  args.chunk_size))


if __name__ == "__main__":
    main()
