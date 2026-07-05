#!/usr/bin/env python3
"""Standalone Ethernet OTA uploader (also available via tcp_master.py ota)."""

from __future__ import annotations

import argparse
import hashlib
import socket
import struct
import sys
from pathlib import Path

MAGIC = 0x45544F41  # ETOA
VERSION = 1
HEADER_FMT = "!IHHI32s"
HEADER_LEN = struct.calcsize(HEADER_FMT)
DEFAULT_IMAGE = Path(__file__).resolve().parent / "build" / "freertos_tasks.bin"


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload an ESP-IDF app image to the board over TCP OTA.")
    parser.add_argument("host", help="Board IPv4 address")
    parser.add_argument(
        "image",
        type=Path,
        nargs="?",
        default=DEFAULT_IMAGE,
        help=f"Path to the ESP-IDF app .bin (default: {DEFAULT_IMAGE})",
    )
    parser.add_argument("--port", type=int, default=3232, help="OTA TCP port")
    parser.add_argument("--timeout", type=float, default=120.0, help="Socket timeout in seconds")
    parser.add_argument("--chunk-size", type=int, default=4096, help="Upload chunk size")
    args = parser.parse_args()

    image_bytes = args.image.read_bytes()
    digest = hashlib.sha256(image_bytes).digest()
    header = struct.pack(HEADER_FMT, MAGIC, VERSION, HEADER_LEN, len(image_bytes), digest)

    print(f"Uploading {args.image} ({len(image_bytes)} bytes) to {args.host}:{args.port}")
    with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        sock.sendall(header)
        for offset in range(0, len(image_bytes), args.chunk_size):
            sock.sendall(image_bytes[offset : offset + args.chunk_size])
            done = min(offset + args.chunk_size, len(image_bytes))
            pct = done * 100 / len(image_bytes)
            print(f"\r{done}/{len(image_bytes)} bytes ({pct:5.1f}%)", end="", flush=True)
        print()

        try:
            response = sock.recv(128).decode("utf-8", errors="replace").strip()
        except socket.timeout:
            response = "<no response before timeout>"
        print(response)
        return 0 if response.startswith("OK") else 1


if __name__ == "__main__":
    sys.exit(main())
