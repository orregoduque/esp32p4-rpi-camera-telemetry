"""
Supabase storage + metadata helpers for TCP camera uploads.

Credentials and behavior adapted from extra_code/supabase.py.
"""

from __future__ import annotations

from pathlib import Path

from supabase import Client, create_client

# From extra_code/supabase.py
SUPABASE_URL = "https://zmxppbpywizdjlhstfkf.supabase.co"
SUPABASE_KEY = (
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InpteHBwYnB5d2l6ZGpsaHN0ZmtmIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjMzNzY4MTksImV4cCI6MjA3ODk1MjgxOX0.AR2B4sNbkf0A_Sm_M-YFtR25ySZMP1FsM93acnuVYWc"
)
SUPABASE_BUCKET = "images"
USER_ID = "b543f74d-bc3e-4c61-b2f4-c19ff65e75e1"


def upload_to_supabase(filepath: str | Path, img_number: int, is_enhanced: bool = False) -> tuple[bool, str | None, int]:
    """Upload image file to Supabase storage."""
    path = Path(filepath)
    try:
        supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)
        file_data = path.read_bytes()

        suffix = "enhanced" if is_enhanced else "raw"
        filename = f"image_{img_number:03d}_{suffix}.jpg"

        supabase.storage.from_(SUPABASE_BUCKET).upload(
            path=filename,
            file=file_data,
            file_options={"content-type": "image/jpeg", "upsert": "true"},
        )

        print(f"Uploaded to Supabase storage: {filename} ({len(file_data)} bytes)")
        return True, filename, len(file_data)
    except Exception as exc:
        print(f"Supabase upload error: {exc}")
        return False, None, 0


def insert_image_metadata(
    img_number: int,
    filename_raw: str,
    filename_enhanced: str,
    storage_path_raw: str,
    storage_path_enhanced: str,
    file_size_raw: int,
    file_size_enhanced: int,
) -> bool:
    """Insert image metadata into Supabase database."""
    try:
        supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)
        data = {
            "image_number": img_number,
            "filename_raw": filename_raw,
            "filename_enhanced": filename_enhanced,
            "storage_path_raw": storage_path_raw,
            "storage_path_enhanced": storage_path_enhanced,
            "file_size_raw": file_size_raw,
            "file_size_enhanced": file_size_enhanced,
            "user_id": USER_ID,
        }
        supabase.table("images").insert(data).execute()
        print(f"Image metadata inserted: image #{img_number}")
        return True
    except Exception as exc:
        print(f"Database insert error: {exc}")
        return False
