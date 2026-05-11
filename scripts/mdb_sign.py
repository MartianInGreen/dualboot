#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 The Official Mesh Admin

"""mesh-dualboot net-OTA signing tool.

Two subcommands:
  keygen   — one-time: generate an Ed25519 keypair, print the pubkey hex
             that goes into MDB_NET_OTA_PUBKEY_HEX.
  sign     — wrap a firmware bin into the net-OTA wire format using
             either HMAC-SHA256 (per-session/NVS PSK) or Ed25519
             (private-key signed; baked-in pubkey on the device verifies).

Wire format (must stay in sync with selector/src/net_ota/mdb_net_ota.cpp):
  Fixed prefix (12 bytes):
    magic[4]      = "MDBO"
    version       = 0x01
    auth_type     = 0x01 (HMAC-SHA256) | 0x02 (Ed25519)
    target_slot   = 0x00 | 0x01
    reserved      = 0x00
    payload_len   = uint32 BE

  Auth blob:
    HMAC mode    (32 bytes): HMAC-SHA256(key, sha256(prefix || payload))
                             where key = HMAC-SHA256(psk_utf8,
                                                     "mdb-netota-auth-v1")
    Ed25519 mode (64 bytes): Ed25519 sign(priv, sha256(prefix || payload))

  Payload:
    payload_len bytes — raw firmware
"""

import argparse
import hashlib
import hmac
import os
import struct
import sys

# Wire-format constants — must match mdb_net_ota.cpp / mdb_net_ota.h.
MAGIC = b"MDBO"
VERSION = 0x01
AUTH_HMAC = 0x01
AUTH_ED25519 = 0x02
HMAC_INFO = b"mdb-netota-auth-v1"


def build_prefix(auth_type: int, target_slot: int, payload_len: int) -> bytes:
    if target_slot not in (0, 1):
        sys.exit(f"target_slot must be 0 or 1, got {target_slot}")
    if payload_len <= 0 or payload_len > 0xFFFFFFFF:
        sys.exit(f"payload_len out of range: {payload_len}")
    # >4sBBBBI = magic[4] | version | auth_type | slot | reserved | payload_len
    return struct.pack(
        ">4sBBBBI",
        MAGIC, VERSION, auth_type, target_slot, 0, payload_len,
    )


def derive_hmac_key(psk: str) -> bytes:
    """Match mdb_net_ota.cpp::derive_hmac_key — HMAC-SHA256(psk, info)."""
    return hmac.new(psk.encode("utf-8"), HMAC_INFO, hashlib.sha256).digest()


def hash_prefix_and_payload(prefix: bytes, payload: bytes) -> bytes:
    h = hashlib.sha256()
    h.update(prefix)
    h.update(payload)
    return h.digest()


def sign_hmac(prefix: bytes, payload: bytes, psk: str) -> bytes:
    digest = hash_prefix_and_payload(prefix, payload)
    key = derive_hmac_key(psk)
    return hmac.new(key, digest, hashlib.sha256).digest()


def _import_cryptography():
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey,
        )
        from cryptography.hazmat.primitives import serialization
        return Ed25519PrivateKey, serialization
    except ImportError:
        sys.exit("Ed25519 mode needs the 'cryptography' package: "
                 "pip install cryptography")


def sign_ed25519(prefix: bytes, payload: bytes, priv_raw: bytes) -> bytes:
    Ed25519PrivateKey, _ = _import_cryptography()
    if len(priv_raw) != 32:
        sys.exit(f"--priv must be 32 raw bytes, got {len(priv_raw)}")
    digest = hash_prefix_and_payload(prefix, payload)
    priv = Ed25519PrivateKey.from_private_bytes(priv_raw)
    return priv.sign(digest)


def cmd_keygen(args):
    Ed25519PrivateKey, serialization = _import_cryptography()
    priv = Ed25519PrivateKey.generate()
    priv_raw = priv.private_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PrivateFormat.Raw,
        encryption_algorithm=serialization.NoEncryption(),
    )
    pub_raw = priv.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )

    # Mode 0600 keeps the private key from being world-readable. Don't
    # use the standard `with open(..., 'wb')` because that creates the
    # file with default permissions before the os.chmod, briefly
    # exposing it.
    fd = os.open(args.priv_out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, priv_raw)
    finally:
        os.close(fd)
    print(f"[mdb_sign keygen] private key (32 raw bytes, mode 0600) -> {args.priv_out}")

    if args.pub_out:
        with open(args.pub_out, "wb") as f:
            f.write(pub_raw)
        print(f"[mdb_sign keygen] public key (32 raw bytes) -> {args.pub_out}")

    print("[mdb_sign keygen] public key hex (paste into MDB_NET_OTA_PUBKEY_HEX):")
    print(f"  {pub_raw.hex()}")


def cmd_sign(args):
    if args.hmac == args.ed25519:
        sys.exit("specify exactly one of --hmac or --ed25519")

    with open(args.input, "rb") as f:
        payload = f.read()
    if not payload:
        sys.exit(f"{args.input} is empty")

    if args.hmac:
        if not args.psk:
            sys.exit("--hmac requires --psk")
        prefix = build_prefix(AUTH_HMAC, args.slot, len(payload))
        auth_blob = sign_hmac(prefix, payload, args.psk)
        mode_label = "HMAC"
    else:
        if not args.priv:
            sys.exit("--ed25519 requires --priv")
        with open(args.priv, "rb") as f:
            priv_raw = f.read()
        prefix = build_prefix(AUTH_ED25519, args.slot, len(payload))
        auth_blob = sign_ed25519(prefix, payload, priv_raw)
        mode_label = "Ed25519"

    out = prefix + auth_blob + payload
    if args.output == "-":
        sys.stdout.buffer.write(out)
        return
    with open(args.output, "wb") as f:
        f.write(out)
    print(f"[mdb_sign sign] {mode_label}, slot {args.slot}")
    print(f"                prefix={len(prefix)} auth={len(auth_blob)} "
          f"payload={len(payload):,} bytes")
    print(f"                wrote {len(out):,} bytes to {args.output}")


def main():
    ap = argparse.ArgumentParser(
        description="mesh-dualboot net-OTA signing (keygen + wire-format wrap)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    kg = sub.add_parser("keygen", help="generate an Ed25519 keypair")
    kg.add_argument("--priv-out", required=True,
                    help="path to write the 32-byte raw private key (mode 0600)")
    kg.add_argument("--pub-out",
                    help="optional path to also write the 32-byte raw public key")
    kg.set_defaults(func=cmd_keygen)

    sg = sub.add_parser("sign", help="sign a firmware bin into the net-OTA wire format")
    mode = sg.add_mutually_exclusive_group(required=True)
    mode.add_argument("--hmac", action="store_true",
                      help="use HMAC-SHA256 with --psk (per-session or NVS-stored PSK)")
    mode.add_argument("--ed25519", action="store_true",
                      help="use Ed25519 with --priv (build-time-baked pubkey verifies)")
    sg.add_argument("--psk",
                    help="HMAC PSK string (matches what's shown on OLED or stored in NVS)")
    sg.add_argument("--priv",
                    help="Ed25519 private key file (32 raw bytes from `keygen`)")
    sg.add_argument("--slot", type=int, required=True, choices=[0, 1],
                    help="target ota slot (must match the device's menu lock when set)")
    sg.add_argument("--in", required=True, dest="input",
                    help="input firmware bin")
    sg.add_argument("--out", required=True, dest="output",
                    help="output signed bin (use '-' for stdout)")
    sg.set_defaults(func=cmd_sign)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
