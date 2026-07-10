#!/usr/bin/env python3
"""
Upload received TCP camera photos to Supabase, always 2 sequences behind the latest.

When tcp_receiver.py saves seq 140, this script uploads seq 138 (once). That lag
avoids racing the receiver while it is still writing files.

Node 1 only for now. Run in a separate shell alongside tcp_receiver.py:

    python tcp_receiver.py --psk "CHANGE_ME_TO_RANDOM_32PLUS_BYTES" --client-timeout 30
    python tcp_supabase_uploader.py

Optional:
    python tcp_supabase_uploader.py --node 1 --lag 2 --poll 2.0
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from supabase_client import insert_image_metadata, upload_to_supabase

DEFAULT_OUT_DIR = Path("received_packets")
STATE_FILENAME = ".supabase_upload_state.json"


def load_state(state_path: Path) -> set[int]:
    if not state_path.is_file():
        return set()
    try:
        data = json.loads(state_path.read_text(encoding="utf-8"))
        return set(int(x) for x in data.get("uploaded_sequences", []))
    except (json.JSONDecodeError, OSError, TypeError, ValueError):
        return set()


def save_state(state_path: Path, uploaded: set[int]) -> None:
    state_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {"uploaded_sequences": sorted(uploaded)}
    state_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def read_latest_sequence(node_dir: Path) -> int | None:
    latest_meta = node_dir / "latest.json"
    if not latest_meta.is_file():
        return None
    try:
        meta = json.loads(latest_meta.read_text(encoding="utf-8"))
        return int(meta["sequence"])
    except (json.JSONDecodeError, OSError, KeyError, TypeError, ValueError):
        return None


def upload_sequence(node_dir: Path, sequence: int) -> bool:
    jpg_path = node_dir / f"seq_{sequence}.jpg"
    if not jpg_path.is_file():
        print(f"[SKIP] seq_{sequence}.jpg not on disk yet")
        return False

    raw_filename_local = jpg_path.name
    enhanced_filename_local = f"{jpg_path.stem}_enhanced{jpg_path.suffix}"

    raw_ok, storage_path_raw, raw_size = upload_to_supabase(jpg_path, sequence, is_enhanced=False)
    if not raw_ok or not storage_path_raw:
        return False

    # TCP receiver stores one JPEG per sequence; upload the same file as enhanced too.
    enh_ok, storage_path_enhanced, enhanced_size = upload_to_supabase(jpg_path, sequence, is_enhanced=True)
    if not enh_ok or not storage_path_enhanced:
        return False

    return insert_image_metadata(
        img_number=sequence,
        filename_raw=raw_filename_local,
        filename_enhanced=enhanced_filename_local,
        storage_path_raw=storage_path_raw,
        storage_path_enhanced=storage_path_enhanced,
        file_size_raw=raw_size,
        file_size_enhanced=enhanced_size,
    )


def run_once(node_dir: Path, state_path: Path, lag: int, uploaded: set[int]) -> int:
    latest = read_latest_sequence(node_dir)
    if latest is None:
        return 0

    target = latest - lag
    if target < 1:
        return 0

    if target in uploaded:
        return 0

    print(f"[UPLOAD] latest_seq={latest} -> uploading seq={target} (lag={lag})")
    if upload_sequence(node_dir, target):
        uploaded.add(target)
        save_state(state_path, uploaded)
        return 1

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Upload TCP receiver photos to Supabase (lagged behind latest sequence)"
    )
    parser.add_argument("--node", type=int, default=1, help="Node ID to watch (default: 1)")
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT_DIR,
        help="Same output dir as tcp_receiver.py (default: received_packets)",
    )
    parser.add_argument(
        "--lag",
        type=int,
        default=2,
        help="Upload (latest_sequence - lag); default 2",
    )
    parser.add_argument(
        "--poll",
        type=float,
        default=2.0,
        help="Poll interval in seconds (default: 2.0)",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="Run one check and exit (default: loop forever)",
    )
    args = parser.parse_args()

    if args.lag < 1:
        print("[!] --lag must be >= 1", file=sys.stderr)
        return 1

    node_dir = args.out / f"node_{args.node}"
    state_path = node_dir / STATE_FILENAME
    uploaded = load_state(state_path)

    print(f"Watching {node_dir.resolve()}")
    print(f"Upload target: latest_sequence - {args.lag}")
    print(f"Already uploaded: {len(uploaded)} sequence(s)")
    print("Press Ctrl+C to stop.\n")

    node_dir.mkdir(parents=True, exist_ok=True)

    try:
        while True:
            run_once(node_dir, state_path, args.lag, uploaded)
            if args.once:
                break
            time.sleep(args.poll)
    except KeyboardInterrupt:
        print("\n[+] Supabase uploader stopped")

    return 0


if __name__ == "__main__":
    sys.exit(main())
