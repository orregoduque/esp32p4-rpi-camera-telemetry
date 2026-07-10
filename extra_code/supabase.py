
# Supabase configuration
SUPABASE_URL = "https://zmxppbpywizdjlhstfkf.supabase.co"
SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InpteHBwYnB5d2l6ZGpsaHN0ZmtmIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjMzNzY4MTksImV4cCI6MjA3ODk1MjgxOX0.AR2B4sNbkf0A_Sm_M-YFtR25ySZMP1FsM93acnuVYWc"
SUPABASE_BUCKET = "images"  # Storage bucket name (adjust if different)

# NEW: static user id to associate with each image row
USER_ID = "b543f74d-bc3e-4c61-b2f4-c19ff65e75e1"

import serial
import base64
import datetime
import re
import os
import subprocess
import cv2
import numpy as np
from supabase import create_client, Client


# ------------------------------------------------------------------
# Helper: upload image to Supabase storage
# ------------------------------------------------------------------
def upload_to_supabase(filepath: str, img_number: int, is_enhanced: bool = False):
    """Upload image file to Supabase storage."""
    try:
        supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)
        
        # Read file
        with open(filepath, 'rb') as f:
            file_data = f.read()
        
        # Generate filename for Supabase
        suffix = "enhanced" if is_enhanced else "raw"
        filename = f"image_{img_number:03d}_{suffix}.jpg"
        
        # Upload to storage bucket
        response = supabase.storage.from_(SUPABASE_BUCKET).upload(
            path=filename,
            file=file_data,
            file_options={"content-type": "image/jpeg", "upsert": "true"}
        )
        
        print(f"Uploaded to Supabase: {filename}")
        log_msg(f"SUPABASE_UPLOAD: {filename} (size: {len(file_data)} bytes)")
        return True, filename, len(file_data)
        
    except Exception as e:
        print(f"Supabase upload error: {e}")
        log_msg(f"SUPABASE_ERROR: {e}")
        return False, None, 0

# ------------------------------------------------------------------
# Helper: insert image metadata into database
# ------------------------------------------------------------------
def insert_image_metadata(img_number: int, filename_raw: str, filename_enhanced: str,
                         storage_path_raw: str, storage_path_enhanced: str,
                         file_size_raw: int, file_size_enhanced: int):
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
            # NEW: associate each row with the fixed user id
            "user_id": USER_ID,
        }
        
        response = supabase.table("images").insert(data).execute()
        
        print(f"Image metadata inserted into database: image #{img_number}")
        log_msg(f"DB_INSERT: image #{img_number} metadata saved")
        return True
        
    except Exception as e:
        print(f"Database insert error: {e}")
        log_msg(f"DB_ERROR: {e}")
        return False
