#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 The Official Mesh Admin

"""mesh-dualboot net-OTA — operator-side trigger via Meshtastic.

Sends commands to the firmware-side DualbootControlModule on private
port 368 so an operator can stage WiFi creds and/or open a net-flash
listener window without physical access to the device. Wraps the
`meshtastic` PyPI package for transport (USB serial / TCP / BLE).

After this script returns successfully on `request`, the device has
rebooted into the selector and is listening on TCP/5050 for an
authenticated bin pushed via mdb_push.py.

## Wire protocol — must stay in sync with firmware/PRIVATE_PORTS.md
##                  and firmware/src/modules/DualbootControlModule.cpp.

Payload is raw bytes inside the MeshPacket data payload (NOT a
sub-protobuf). All multi-byte ints are little-endian.

  Requests:
    0x01 SET_WIFI:           [0x01, ssid_len, ssid..., psk_len, psk...]
    0x02 REQUEST_NET_FLASH:  [0x02, ssid_len, ssid..., psk_len, psk...,
                              timeout_lo, timeout_hi]   (timeout u16 LE)

  Replies:
    0x81 SET_WIFI_REPLY:     [0x81, status]      (always)
    0x82 REQUEST_REPLY:      [0x82, status]      (only on FAILURE)

  status: 0=OK, 1=NVS_WRITE_FAILED, 2=BAD_REQUEST

For `set-wifi`: always expect a 0x81 reply.
For `request`: success = no reply + radio link drops within ~50 ms
(device rebooted into selector). Failure = 0x82 reply within tens of ms.
"""

import argparse
import struct
import sys
import threading

# ── Wire format ─────────────────────────────────────────────────────────
DUALBOOT_PORT = 368
CMD_SET_WIFI = 0x01
CMD_REQUEST_NET_FLASH = 0x02
REPLY_SET_WIFI = 0x81
REPLY_REQUEST = 0x82

STATUS_NAMES = {
    0x00: "OK",
    0x01: "NVS_WRITE_FAILED (selector partition init or write failed)",
    0x02: "BAD_REQUEST (malformed body, length-prefix overrun, or empty SSID)",
    0x03: "FORBIDDEN_VIA_MESH (command must arrive over USB serial or BLE, "
          "not over LoRa-mesh or MQTT — use --serial or --ble, or --host "
          "without --dest so the connected node is the target)",
}

# 802.11 limits, also enforced by the firmware module.
SSID_MAX = 32   # 802.11 SSID octet maximum
PSK_MAX  = 64   # WPA2 64-hex (or 8-63 ASCII passphrase) — firmware buffer is 64


def _check_ssid(ssid: str) -> bytes:
    b = ssid.encode("utf-8")
    if not (1 <= len(b) <= SSID_MAX):
        sys.exit(f"--ssid must be 1..{SSID_MAX} UTF-8 bytes (got {len(b)})")
    return b


def _check_psk(psk: str) -> bytes:
    b = psk.encode("utf-8")
    if len(b) > PSK_MAX:
        sys.exit(f"--psk must be at most {PSK_MAX} UTF-8 bytes (got {len(b)})")
    return b


def encode_set_wifi(ssid: str, psk: str) -> bytes:
    s, p = _check_ssid(ssid), _check_psk(psk)
    return bytes([CMD_SET_WIFI, len(s)]) + s + bytes([len(p)]) + p


def encode_request(ssid: str, psk: str, timeout_s: int) -> bytes:
    s, p = _check_ssid(ssid), _check_psk(psk)
    if not (0 <= timeout_s <= 0xFFFF):
        sys.exit(f"--timeout-s out of range (0..65535): {timeout_s}")
    return (bytes([CMD_REQUEST_NET_FLASH, len(s)]) + s
            + bytes([len(p)]) + p
            + struct.pack("<H", timeout_s))


# ── Reply listener ──────────────────────────────────────────────────────
class ReplyListener:
    """Captures the first inbound packet on DUALBOOT_PORT into self.payload.

    The meshtastic Python package fires `meshtastic.receive.data` when a
    data packet arrives. portnum in the decoded packet may be an int
    (private ports above PRIVATE_APP) or a string-enum (well-known ports).
    """
    def __init__(self):
        self.event = threading.Event()
        self.payload = None

    def __call__(self, packet=None, interface=None):
        if self.event.is_set():
            return
        try:
            decoded = (packet or {}).get('decoded') or {}
            portnum = decoded.get('portnum')
            port_int = None
            if isinstance(portnum, int):
                port_int = portnum
            elif isinstance(portnum, str) and portnum.isdigit():
                port_int = int(portnum)
            if port_int != DUALBOOT_PORT:
                return
            payload = decoded.get('payload')
            if isinstance(payload, (bytes, bytearray)) and len(payload) >= 1:
                self.payload = bytes(payload)
                self.event.set()
        except Exception:
            pass


# ── Transport ───────────────────────────────────────────────────────────
def _open_interface(args):
    try:
        from meshtastic.serial_interface import SerialInterface
        from meshtastic.tcp_interface import TCPInterface
        from pubsub import pub
    except ImportError:
        sys.exit("This script needs the meshtastic package: "
                 "pip install meshtastic")

    if args.host:
        return TCPInterface(hostname=args.host), pub
    if args.serial:
        return SerialInterface(devPath=args.serial), pub
    if args.ble:
        try:
            from meshtastic.ble_interface import BLEInterface
        except ImportError:
            sys.exit("--ble requires the BLE extra: "
                     "pip install 'meshtastic[ble]'")
        return BLEInterface(args.ble), pub
    sys.exit("specify --host (TCP), --serial (USB), or --ble")


def _resolve_dest(iface, args):
    """Pick the destination nodeNum.

    For all three transports, the connected device IS the target by
    default — it's the dualboot device itself. --dest overrides this
    when the operator is reaching the target via a relay node.
    """
    if args.dest is not None:
        # Accept "0x12345678", "!12345678" (Meshtastic form), or decimal.
        s = args.dest.lstrip("!")
        if s.lower().startswith("0x"):
            return int(s, 16)
        try:
            return int(s, 10)
        except ValueError:
            return int(s, 16)  # bare hex like "12345678"
    info = iface.getMyNodeInfo()
    # NodeInfo can be dict or proto-message depending on package version.
    return info['num'] if isinstance(info, dict) else info.num


# ── Subcommands ─────────────────────────────────────────────────────────
def _send_and_wait(args, body: bytes, expected_reply: int, label: str,
                   no_reply_means_success: bool) -> int:
    iface, pub = _open_interface(args)
    listener = ReplyListener()
    # Subscribe BEFORE sending so a fast reply can't race past us.
    pub.subscribe(listener, "meshtastic.receive.data")
    pub.subscribe(listener, "meshtastic.receive")  # belt-and-suspenders for older versions
    try:
        dest = _resolve_dest(iface, args)
        print(f"[mdb_request_net_flash] {label} -> nodeNum {dest:#x}, "
              f"port {DUALBOOT_PORT}, body {len(body)} bytes")
        # wantAck=False: device replies via the application-level 0x81/0x82
        # reply, not the transport ack. For `request`, the device reboots
        # before any ack would be generated anyway.
        iface.sendData(body, destinationId=dest, portNum=DUALBOOT_PORT,
                       wantAck=False)
        if listener.event.wait(timeout=args.reply_timeout):
            return _interpret_reply(listener.payload, expected_reply, label)
        if no_reply_means_success:
            print("status: OK  (no reply within window — device rebooted)")
            return 0
        sys.exit(f"timeout: no reply on port {DUALBOOT_PORT} "
                 f"within {args.reply_timeout}s")
    finally:
        try:
            iface.close()
        except Exception:
            pass  # device may have rebooted out from under us — fine


def _interpret_reply(payload: bytes, expected_cmd: int, label: str) -> int:
    if not payload:
        sys.exit(f"{label}: empty reply")
    cmd, rest = payload[0], payload[1:]
    if cmd != expected_cmd:
        sys.exit(f"{label}: unexpected reply byte 0x{cmd:02x} "
                 f"(expected 0x{expected_cmd:02x})")
    if not rest:
        sys.exit(f"{label}: short reply (1 byte; expected 2)")
    status = rest[0]
    name = STATUS_NAMES.get(status, f"unknown 0x{status:02x}")
    if status == 0:
        print("status: OK")
        return 0
    print(f"status: {name}")
    return 1


def _dry_run(body: bytes, label: str) -> int:
    print(f"[mdb_request_net_flash dry-run] {label} ({len(body)} bytes)")
    print(f"  hex:    {body.hex()}")
    print(f"  bytes:  {' '.join(f'{b:02x}' for b in body)}")
    print(f"  port:   {DUALBOOT_PORT}")
    return 0


def cmd_set_wifi(args):
    body = encode_set_wifi(args.ssid, args.psk)
    if args.dry_run:
        return _dry_run(body, "SET_WIFI")
    return _send_and_wait(args, body, REPLY_SET_WIFI, "SET_WIFI",
                          no_reply_means_success=False)


def cmd_request(args):
    body = encode_request(args.ssid, args.psk, args.timeout_s)
    if args.dry_run:
        return _dry_run(body, "REQUEST_NET_FLASH")
    return _send_and_wait(args, body, REPLY_REQUEST, "REQUEST_NET_FLASH",
                          no_reply_means_success=True)


# ── CLI ─────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(
        description="mesh-dualboot net-OTA: trigger via Meshtastic admin",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Workflow:\n"
            "  1. mdb_request_net_flash.py --serial /dev/cu.usbmodem* set-wifi --ssid bellona --psk takloufer\n"
            "     (or skip this if you'll send creds inline with the request)\n"
            "  2. mdb_request_net_flash.py --serial /dev/cu.usbmodem* request \\\n"
            "        --ssid bellona --psk takloufer --timeout-s 300\n"
            "     (device reboots into selector net-flash mode)\n"
            "  3. mdb_sign.py sign --hmac --psk takloufer --slot 1 --in fw.bin --out fw.signed\n"
            "  4. mdb_push.py --host mdb-XXYY.local --bin fw.signed\n"
        ),
    )
    transport = ap.add_mutually_exclusive_group()
    transport.add_argument("--host",
                           help="TCP hostname/IP of meshtastic node")
    transport.add_argument("--serial",
                           help="USB serial device path (e.g., /dev/cu.usbmodem*)")
    transport.add_argument("--ble",
                           help="BLE node name or MAC")
    ap.add_argument("--dest",
                    help="Override destination nodeNum (default: connected node's own ID). "
                         "Accepts decimal, 0x-prefixed hex, or '!<hex>' Meshtastic form.")
    ap.add_argument("--reply-timeout", type=float, default=5.0,
                    help="Seconds to wait for reply / disconnect (default: 5).")
    ap.add_argument("--dry-run", action="store_true",
                    help="Don't connect; just print the encoded payload.")

    sub = ap.add_subparsers(dest="cmd", required=True)

    sw = sub.add_parser("set-wifi",
                        help="persist WiFi creds in nvs_sel without rebooting")
    sw.add_argument("--ssid", required=True)
    sw.add_argument("--psk",  required=True)
    sw.set_defaults(func=cmd_set_wifi)

    rq = sub.add_parser("request",
                        help="persist creds + open net-flash window (reboots device)")
    rq.add_argument("--ssid", required=True)
    rq.add_argument("--psk",  required=True)
    rq.add_argument("--timeout-s", type=int, default=300, dest="timeout_s",
                    help="selector listener window in seconds (default: 300)")
    rq.set_defaults(func=cmd_request)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
