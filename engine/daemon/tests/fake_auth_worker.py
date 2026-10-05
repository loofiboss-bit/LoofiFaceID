#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Non-installed process fixture for daemon unit tests."""
import os
from pathlib import Path
import sys
import time

request = sys.stdin.buffer.read()
if len(request) != 8:
    sys.exit(2)
mode = os.environ["KFACEAUTH_CAMERA_DEVICE"]
root = Path(os.environ["KFACEAUTH_KEYS_DIR"])
response = bytes([0, 1, 0, 0])
if mode == "hang-once" and not (root / "started").exists():
    (root / "started").write_text("started")
    time.sleep(30)
if mode == "late-success":
    sys.stdout.buffer.write(response)
    sys.stdout.buffer.flush()
    time.sleep(30)
if mode == "crash":
    sys.exit(3)
if mode == "trailing":
    response += b"x"
if mode == "reserved":
    response = bytes([0, 1, 0, 1])
if mode == "version":
    response = bytes([0, 2, 0, 0])
if mode == "status":
    response = bytes([0, 1, 255, 0])
if mode == "truncated":
    response = response[:3]
sys.stdout.buffer.write(response)
