#!/usr/bin/env python3
"""
ethernet on pc: 192.168.1.10
subnet mask: 255.255.255.0

python tcp_receiver.py --psk "CHANGE_ME_TO_RANDOM_32PLUS_BYTES" --client-timeout 30


TCP receiver for ESP32-P4 camera telemetry packets.

Features:
- Receives packet header + JPEG payload
- Verifies magic/version/size
- Verifies CRC32 of JPEG
- Verifies HMAC-SHA256 (PSK-based)
- Sends ACK with sequence + status
- Stores latest image and metadata per node

Run from CMD (direct cable — set the PC Ethernet adapter to 192.168.1.10 / 255.255.255.0):
    python tcp_receiver.py --bind 0.0.0.0 --port 9000 --psk "CHANGE_ME_TO_RANDOM_32PLUS_BYTES"
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import hmac
import json
import os
import socket
import struct
import threading
import time
import zlib
from pathlib import Path
from typing import Optional


MAGIC = 0x5250434D  # "RPCM"
VERSION = 1
HMAC_SIZE = 32

# Network byte order, packed:
# uint32 magic
# uint16 version
# uint16 header_size
# uint32 node_id
# uint32 sequence
# uint64 timestamp_ms
# int16  temperature_centi_c
# uint16 reserved
# uint32 jpeg_size
# uint32 jpeg_crc32
# uint8  hmac[32]
HEADER_FMT = "!I H H I I Q h H I I 32s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
ACK_FMT = "!I I I"  # magic, sequence, status
ACK_SIZE = struct.calcsize(ACK_FMT)

STATUS_OK = 0
STATUS_BAD_MAGIC = 1
STATUS_BAD_VERSION = 2
STATUS_BAD_HEADER = 3
STATUS_BAD_SIZE = 4
STATUS_BAD_CRC = 5
STATUS_BAD_HMAC = 6
STATUS_INTERNAL_ERROR = 7

RPI_PKT_PAYLOAD_VISIBLE = 0
RPI_PKT_PAYLOAD_THERMAL = 1

STATUS_LABELS = {
    STATUS_OK: "OK",
    STATUS_BAD_MAGIC: "BAD_MAGIC",
    STATUS_BAD_VERSION: "BAD_VERSION",
    STATUS_BAD_HEADER: "BAD_HEADER",
    STATUS_BAD_SIZE: "BAD_SIZE",
    STATUS_BAD_CRC: "BAD_CRC",
    STATUS_BAD_HMAC: "BAD_HMAC",
    STATUS_INTERNAL_ERROR: "INTERNAL_ERROR",
}


def local_ipv4_addresses() -> list[str]:
    addrs: list[str] = []
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            ip = info[4][0]
            if ip not in addrs:
                addrs.append(ip)
    except OSError:
        pass

    # Best-effort: route used to reach the ESP subnet (direct cable setup).
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("192.168.1.20", 9))
        ip = probe.getsockname()[0]
        probe.close()
        if ip not in addrs:
            addrs.insert(0, ip)
    except OSError:
        pass
    return addrs


class Diagnostics:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.total_packets = 0
        self.total_bytes = 0
        self.client_connects = 0
        self.client_disconnects = 0
        self.node_last_seq: dict[int, int] = {}
        self.status_counts = collections.Counter()
        self.last_error = ""

    def note_connect(self) -> None:
        with self.lock:
            self.client_connects += 1

    def note_disconnect(self) -> None:
        with self.lock:
            self.client_disconnects += 1

    def note(self, status: int, node_id: int | None = None, seq: int | None = None, nbytes: int = 0, error: str = "") -> None:
        with self.lock:
            self.status_counts[status] += 1
            if status == STATUS_OK:
                self.total_packets += 1
                self.total_bytes += nbytes
                if node_id is not None and seq is not None:
                    self.node_last_seq[node_id] = seq
            if error:
                self.last_error = error

    def snapshot(self) -> dict:
        with self.lock:
            return {
                "total_packets": self.total_packets,
                "total_bytes": self.total_bytes,
                "client_connects": self.client_connects,
                "client_disconnects": self.client_disconnects,
                "status_counts": dict(self.status_counts),
                "node_last_seq": dict(self.node_last_seq),
                "last_error": self.last_error,
            }


def diagnostics_loop(diag: Diagnostics, period_s: int) -> None:
    while True:
        time.sleep(period_s)
        snap = diag.snapshot()
        status_human = {STATUS_LABELS.get(k, str(k)): v for k, v in snap["status_counts"].items()}
        print(
            "[DIAG] packets=%d bytes=%d connects=%d disconnects=%d status=%s node_last_seq=%s last_error=%s"
            % (
                snap["total_packets"],
                snap["total_bytes"],
                snap["client_connects"],
                snap["client_disconnects"],
                status_human,
                snap["node_last_seq"],
                snap["last_error"] or "-",
            )
        )


def recv_exact(sock: socket.socket, n: int) -> Optional[bytes]:
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data.extend(chunk)
    return bytes(data)


def send_ack(sock: socket.socket, sequence: int, status: int) -> None:
    ack = struct.pack(ACK_FMT, MAGIC, sequence, status)
    sock.sendall(ack)


def compute_expected_hmac(psk: bytes, header_without_hmac: bytes, jpeg_payload: bytes) -> bytes:
    hm = hmac.new(psk, digestmod=hashlib.sha256)
    hm.update(header_without_hmac)
    hm.update(jpeg_payload)
    return hm.digest()


def handle_client(
    sock: socket.socket,
    addr: tuple[str, int],
    psk: bytes,
    out_dir: Path,
    jpeg_max_bytes: int,
    diag: Diagnostics,
    verbose_failures: bool,
    client_timeout: float,
) -> None:
    peer = f"{addr[0]}:{addr[1]}"
    diag.note_connect()
    print(f"[+] Client connected: {peer}")
    # Keep persistent ESP connections alive between 10s snapshots.
    sock.settimeout(client_timeout)

    try:
        while True:
            raw_header = recv_exact(sock, HEADER_SIZE)
            if raw_header is None:
                break

            (
                magic,
                version,
                header_size,
                node_id,
                sequence,
                timestamp_ms,
                temp_centi_c,
                _reserved,
                jpeg_size,
                jpeg_crc32,
                recv_hmac,
            ) = struct.unpack(HEADER_FMT, raw_header)

            if magic != MAGIC:
                send_ack(sock, sequence, STATUS_BAD_MAGIC)
                diag.note(STATUS_BAD_MAGIC, error=f"{peer} bad magic 0x{magic:08x}")
                if verbose_failures:
                    print(f"[WARN] {peer} seq={sequence} invalid magic=0x{magic:08x}")
                continue
            if version != VERSION:
                send_ack(sock, sequence, STATUS_BAD_VERSION)
                diag.note(STATUS_BAD_VERSION, error=f"{peer} bad version {version}")
                if verbose_failures:
                    print(f"[WARN] {peer} seq={sequence} invalid version={version}")
                continue
            if header_size != HEADER_SIZE:
                send_ack(sock, sequence, STATUS_BAD_HEADER)
                diag.note(STATUS_BAD_HEADER, error=f"{peer} bad header_size {header_size}")
                if verbose_failures:
                    print(f"[WARN] {peer} seq={sequence} invalid header_size={header_size}")
                continue
            if jpeg_size <= 0 or jpeg_size > jpeg_max_bytes:
                send_ack(sock, sequence, STATUS_BAD_SIZE)
                diag.note(STATUS_BAD_SIZE, error=f"{peer} bad jpeg_size {jpeg_size}")
                if verbose_failures:
                    print(f"[WARN] {peer} seq={sequence} invalid jpeg_size={jpeg_size}")
                continue

            payload = recv_exact(sock, jpeg_size)
            if payload is None:
                break

            calc_crc = zlib.crc32(payload) & 0xFFFFFFFF
            if calc_crc != jpeg_crc32:
                send_ack(sock, sequence, STATUS_BAD_CRC)
                diag.note(STATUS_BAD_CRC, error=f"{peer} seq={sequence} crc mismatch rx={jpeg_crc32} calc={calc_crc}")
                if verbose_failures:
                    print(f"[WARN] {peer} seq={sequence} CRC mismatch rx={jpeg_crc32} calc={calc_crc}")
                continue

            # Rebuild header with hmac field zeroed (matches sender behavior)
            header_zero_hmac = struct.pack(
                HEADER_FMT,
                magic,
                version,
                header_size,
                node_id,
                sequence,
                timestamp_ms,
                temp_centi_c,
                _reserved,
                jpeg_size,
                jpeg_crc32,
                b"\x00" * HMAC_SIZE,
            )
            expected_hmac = compute_expected_hmac(psk, header_zero_hmac, payload)
            if not hmac.compare_digest(expected_hmac, recv_hmac):
                send_ack(sock, sequence, STATUS_BAD_HMAC)
                diag.note(STATUS_BAD_HMAC, error=f"{peer} seq={sequence} hmac mismatch")
                if verbose_failures:
                    print(f"[WARN] {peer} seq={sequence} HMAC mismatch (check PSK)")
                continue

            node_dir = out_dir / f"node_{node_id}"
            node_dir.mkdir(parents=True, exist_ok=True)

            payload_type = _reserved
            is_thermal = payload_type == RPI_PKT_PAYLOAD_THERMAL
            kind = "thermal" if is_thermal else "visible"
            suffix = "_thermal" if is_thermal else ""

            latest_jpg = node_dir / f"latest{suffix}.jpg"
            latest_meta = node_dir / f"latest{suffix}.json"
            archive_jpg = node_dir / f"seq_{sequence}{suffix}.jpg"
            archive_meta = node_dir / f"seq_{sequence}{suffix}.json"

            ts_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(timestamp_ms / 1000.0))
            temperature_c = temp_centi_c / 100.0

            metadata = {
                "node_id": node_id,
                "sequence": sequence,
                "payload_type": payload_type,
                "kind": kind,
                "timestamp_ms": timestamp_ms,
                "timestamp_iso_utc": ts_iso,
                "temperature_c": temperature_c,
                "jpeg_size": jpeg_size,
                "jpeg_crc32": jpeg_crc32,
                "received_at_unix_ms": int(time.time() * 1000),
                "peer": peer,
            }

            # Write atomically enough for operational use
            latest_jpg.write_bytes(payload)
            latest_meta.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
            archive_jpg.write_bytes(payload)
            archive_meta.write_text(json.dumps(metadata, indent=2), encoding="utf-8")

            send_ack(sock, sequence, STATUS_OK)
            diag.note(STATUS_OK, node_id=node_id, seq=sequence, nbytes=jpeg_size)
            print(
                f"[OK] node={node_id} seq={sequence} kind={kind} temp={temperature_c:.2f}C "
                f"jpeg={jpeg_size}B saved={archive_jpg}"
            )

    except socket.timeout:
        diag.note(STATUS_INTERNAL_ERROR, error=f"{peer} timeout")
        print(f"[!] Timeout from {peer}, closing")
    except Exception as exc:
        diag.note(STATUS_INTERNAL_ERROR, error=f"{peer} exception: {exc}")
        print(f"[!] Error with {peer}: {exc}")
        try:
            send_ack(sock, 0, STATUS_INTERNAL_ERROR)
        except Exception:
            pass
    finally:
        sock.close()
        diag.note_disconnect()
        print(f"[-] Client disconnected: {peer}")


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32-P4 telemetry TCP receiver")
    parser.add_argument("--bind", default="0.0.0.0", help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=9000, help="TCP port (default: 9000)")
    parser.add_argument("--psk", required=True, help="Pre-shared key (must match firmware)")
    parser.add_argument(
        "--out",
        default="received_packets",
        help="Output directory for images/metadata (default: received_packets)",
    )
    parser.add_argument(
        "--jpeg-max-bytes",
        type=int,
        default=2_000_000,
        help="Max accepted JPEG payload size",
    )
    parser.add_argument(
        "--diag-interval",
        type=int,
        default=15,
        help="Diagnostics summary print interval in seconds (default: 15)",
    )
    parser.add_argument(
        "--verbose-failures",
        action="store_true",
        help="Print per-packet reason on parse/auth failures",
    )
    parser.add_argument(
        "--client-timeout",
        type=float,
        default=30.0,
        help="Per-client socket read timeout seconds (default: 30.0)",
    )
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(8)

    print(f"Listening on {args.bind}:{args.port}")
    print(f"Output dir: {out_dir.resolve()}")
    local_ips = local_ipv4_addresses()
    if local_ips:
        print(f"This PC IPv4 address(es): {', '.join(local_ips)}")
    else:
        print("This PC IPv4 address: (could not detect — set Ethernet adapter manually)")
    print("Direct cable setup: set the Ethernet adapter used for the ESP to 192.168.1.10 / 255.255.255.0")
    print("ESP firmware connects to 192.168.1.10:9000 (ESP IP is 192.168.1.20)")
    print("Use --client-timeout 30 when ESP send interval is 10 s")
    print("Press Ctrl+C to stop.")

    psk = args.psk.encode("utf-8")
    diag = Diagnostics()
    tdiag = threading.Thread(target=diagnostics_loop, args=(diag, args.diag_interval), daemon=True)
    tdiag.start()

    try:
        while True:
            client, addr = srv.accept()
            t = threading.Thread(
                target=handle_client,
                args=(client, addr, psk, out_dir, args.jpeg_max_bytes, diag, args.verbose_failures, args.client_timeout),
                daemon=True,
            )
            t.start()
    except KeyboardInterrupt:
        print("\nStopping receiver...")
    finally:
        srv.close()


if __name__ == "__main__":
    main()

