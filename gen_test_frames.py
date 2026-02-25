#!/usr/bin/env python3
"""Capture test frames from webcam into data/test_frames/."""
import cv2
import os
import time

NUM_FRAMES = 10
DELAY_BEFORE = 3
INTERVAL = 1.0
OUT_DIR = "data/test_frames"

os.makedirs(OUT_DIR, exist_ok=True)

cap = cv2.VideoCapture(0)
if not cap.isOpened():
    print("ERROR: cannot open /dev/video0")
    exit(1)

w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
print(f"Camera opened: {w}x{h}")
print(f"Waiting {DELAY_BEFORE}s before capture...")
time.sleep(DELAY_BEFORE)

for i in range(NUM_FRAMES):
    ret, frame = cap.read()
    if not ret:
        print(f"ERROR: failed to read frame {i}")
        break
    path = os.path.join(OUT_DIR, f"frame_{i:03d}.jpg")
    cv2.imwrite(path, frame)
    print(f"[{i+1}/{NUM_FRAMES}] Saved {path}")
    if i < NUM_FRAMES - 1:
        time.sleep(INTERVAL)

cap.release()
print(f"Done. {NUM_FRAMES} frames saved to {OUT_DIR}/")
