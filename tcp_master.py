#!/usr/bin/env python3
"""
Master control CLI for ESP32-P4 camera slave nodes.

Connects to the slave control port (default 9001) and sends START / STOP / RESTART / status.

Usage:
    python tcp_master.py --host 192.168.1.20 status
    python tcp_master.py --host 192.168.1.20 start
    python tcp_master.py --host 192.168.1.20 stop
    python tcp_master.py --host 192.168.1.20 restart
    python tcp_master.py --host 192.168.1.20 monitor
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from typing import Optional, Tuple

MAGIC = 0x52504343  # "RPCC"
VERSION = 1

MSG_CMD = 1
MSG_ACK = 2
MSG_STATE = 3

CMD_QUERY_STATE = 1
CMD_START = 2
CMD_STOP = 3
CMD_RESTART = 4

STATUS_OK = 0
STATUS_BAD_MAGIC = 1
STATUS_BAD_VERSION = 2
STATUS_BAD_CMD = 3
STATUS_INVALID_STATE = 4
STATUS_INTERNAL = 5

STATE_NAMES = {
    0: "BOOT",
    1: "NET_INIT",
    2: "IDLE",
    3: "CAM_INIT",
    4: "WARMUP",
    5: "RUNNING",
    6: "PAUSED",
    7: "SPOOL_BACKLOG",
    8: "FAULT",
    9: "RESTARTING",
}

STATUS_NAMES = {
    STATUS_OK: "OK",
    STATUS_BAD_MAGIC: "BAD_MAGIC",
    STATUS_BAD_VERSION: "BAD_VERSION",
    STATUS_BAD_CMD: "BAD_CMD",
    STATUS_INVALID_STATE: "INVALID_STATE",
    STATUS_INTERNAL: "INTERNAL",
}

CMD_FMT = "!I H H I H H I"
ACK_FMT = "!I H H I I H H"
STATE_FMT = "!I H H I H B B H I I I I Q"

CMD_SIZE = struct.calcsize(CMD_FMT)
ACK_SIZE = struct.calcsize(ACK_FMT)
STATE_SIZE = struct.calcsize(STATE_FMT)


def recv_exact(sock: socket.socket, n: int) -> Optional[bytes]:
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data.extend(chunk)
    return bytes(data)


def send_command(sock: socket.socket, cmd_id: int, command: int) -> None:
    pkt = struct.pack(CMD_FMT, MAGIC, VERSION, MSG_CMD, cmd_id, command, 0, 0)
    sock.sendall(pkt)


def parse_ack(raw: bytes) -> Tuple[int, int, int]:
    magic, version, msg_type, cmd_id, status, state, _reserved = struct.unpack(ACK_FMT, raw)
    return cmd_id, status, state


def parse_state(raw: bytes) -> dict:
    (
        magic,
        version,
        msg_type,
        cmd_id,
        state,
        link_up,
        tcp_connected,
        _reserved,
        boot_count,
        sequence,
        spool_pending,
        send_failures,
        uptime_ms,
    ) = struct.unpack(STATE_FMT, raw)
    return {
        "cmd_id": cmd_id,
        "state": state,
        "state_name": STATE_NAMES.get(state, f"UNKNOWN({state})"),
        "link_up": bool(link_up),
        "tcp_connected": bool(tcp_connected),
        "boot_count": boot_count,
        "sequence": sequence,
        "spool_pending": spool_pending,
        "send_failures": send_failures,
        "uptime_ms": uptime_ms,
    }


def format_state(s: dict) -> str:
    uptime_s = s["uptime_ms"] / 1000.0
    return (
        f"state={s['state_name']} boot={s['boot_count']} seq={s['sequence']} "
        f"spool={s['spool_pending']} fails={s['send_failures']} "
        f"link={'up' if s['link_up'] else 'down'} tcp={'yes' if s['tcp_connected'] else 'no'} "
        f"uptime={uptime_s:.1f}s"
    )


def run_command(host: str, port: int, command: int, timeout: float) -> int:
    cmd_id = int(time.time()) & 0xFFFFFFFF
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect((host, port))
        send_command(sock, cmd_id, command)

        ack_raw = recv_exact(sock, ACK_SIZE)
        if ack_raw is None:
            print("[!] no ACK from slave")
            return 1

        rx_cmd_id, status, state = parse_ack(ack_raw)
        print(f"[ACK] cmd_id={rx_cmd_id} status={STATUS_NAMES.get(status, status)} state={STATE_NAMES.get(state, state)}")

        state_raw = recv_exact(sock, STATE_SIZE)
        if state_raw is None:
            print("[!] no STATE after ACK")
            return 1 if status != STATUS_OK else 0

        info = parse_state(state_raw)
        print(f"[STATE] {format_state(info)}")
        return 0 if status == STATUS_OK else 1
    except OSError as exc:
        print(f"[!] connection failed: {exc}")
        return 1
    finally:
        sock.close()


def monitor(host: str, port: int, interval: float) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(interval + 5.0)
    try:
        sock.connect((host, port))
        print(f"[+] monitoring {host}:{port} (Ctrl+C to stop)")
        while True:
            raw = recv_exact(sock, STATE_SIZE)
            if raw is None:
                print("[!] disconnected")
                return 1
            if len(raw) == STATE_SIZE:
                info = parse_state(raw)
                if struct.unpack(STATE_FMT, raw)[2] == MSG_STATE:
                    print(f"[HB] {format_state(info)}")
    except KeyboardInterrupt:
        print("\n[+] monitor stopped")
        return 0
    except OSError as exc:
        print(f"[!] {exc}")
        return 1
    finally:
        sock.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP32-P4 slave master control")
    parser.add_argument("--host", default="192.168.1.20", help="Slave IP (default: 192.168.1.20)")
    parser.add_argument("--port", type=int, default=9001, help="Control port (default: 9001)")
    parser.add_argument("--timeout", type=float, default=10.0, help="Socket timeout seconds")
    parser.add_argument(
        "action",
        choices=["status", "start", "stop", "restart", "monitor"],
        help="Master command",
    )
    args = parser.parse_args()

    if args.action == "monitor":
        return monitor(args.host, args.port, args.timeout)

    cmd_map = {
        "status": CMD_QUERY_STATE,
        "start": CMD_START,
        "stop": CMD_STOP,
        "restart": CMD_RESTART,
    }
    return run_command(args.host, args.port, cmd_map[args.action], args.timeout)


if __name__ == "__main__":
    sys.exit(main())
