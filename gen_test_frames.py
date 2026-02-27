#!/usr/bin/env python3
"""Generate test frames for FILE mode pipeline testing.

Mode 1 (default): Download COCO val2017 sample images (no webcam needed)
Mode 2 (--webcam): Capture from webcam (original behavior)

Usage:
    python gen_test_frames.py              # download COCO samples
    python gen_test_frames.py --webcam     # capture from webcam
"""
import os
import sys
import urllib.request
import cv2
import numpy as np

OUT_DIR = "data/test_frames"

# COCO val2017 sample images — public domain, contain common detectable objects
# Source: http://images.cocodataset.org/val2017/
COCO_SAMPLES = [
    # (filename, url, expected objects)
    ("frame_000.jpg", "http://images.cocodataset.org/val2017/000000039769.jpg", "cats on couch"),
    ("frame_001.jpg", "http://images.cocodataset.org/val2017/000000397133.jpg", "person + dog"),
    ("frame_002.jpg", "http://images.cocodataset.org/val2017/000000522713.jpg", "person + umbrella"),
    ("frame_003.jpg", "http://images.cocodataset.org/val2017/000000011760.jpg", "dining table + food"),
    ("frame_004.jpg", "http://images.cocodataset.org/val2017/000000080671.jpg", "dog"),
    ("frame_005.jpg", "http://images.cocodataset.org/val2017/000000109798.jpg", "bus + people"),
    ("frame_006.jpg", "http://images.cocodataset.org/val2017/000000001268.jpg", "cat"),
    ("frame_007.jpg", "http://images.cocodataset.org/val2017/000000002587.jpg", "pizza"),
    ("frame_008.jpg", "http://images.cocodataset.org/val2017/000000007386.jpg", "elephant"),
    ("frame_009.jpg", "http://images.cocodataset.org/val2017/000000037777.jpg", "giraffe"),
    ("frame_010.jpg", "http://images.cocodataset.org/val2017/000000087038.jpg", "horse + person"),
    ("frame_011.jpg", "http://images.cocodataset.org/val2017/000000127517.jpg", "car + traffic"),
    ("frame_012.jpg", "http://images.cocodataset.org/val2017/000000018150.jpg", "zebra"),
    ("frame_013.jpg", "http://images.cocodataset.org/val2017/000000087470.jpg", "bicycle + person"),
    ("frame_014.jpg", "http://images.cocodataset.org/val2017/000000036936.jpg", "boat"),
    ("frame_015.jpg", "http://images.cocodataset.org/val2017/000000058350.jpg", "airplane"),
    ("frame_016.jpg", "http://images.cocodataset.org/val2017/000000079565.jpg", "bird"),
    ("frame_017.jpg", "http://images.cocodataset.org/val2017/000000099054.jpg", "truck"),
    ("frame_018.jpg", "http://images.cocodataset.org/val2017/000000017178.jpg", "bear"),
    ("frame_019.jpg", "http://images.cocodataset.org/val2017/000000015497.jpg", "bench + person"),
]

TARGET_W, TARGET_H = 640, 480


def download_coco_samples():
    """Download COCO val2017 images and resize to 640x480."""
    os.makedirs(OUT_DIR, exist_ok=True)

    # Clean existing frames
    for f in os.listdir(OUT_DIR):
        if f.endswith(".jpg"):
            os.remove(os.path.join(OUT_DIR, f))
    print(f"Cleaned existing frames in {OUT_DIR}/")

    success = 0
    for filename, url, desc in COCO_SAMPLES:
        out_path = os.path.join(OUT_DIR, filename)
        print(f"  Downloading {filename} ({desc})...", end=" ", flush=True)
        try:
            tmp_path = out_path + ".tmp"
            urllib.request.urlretrieve(url, tmp_path)

            # Read, resize to 640x480, save
            img = cv2.imread(tmp_path)
            if img is None:
                print("FAILED (corrupt)")
                os.remove(tmp_path)
                continue

            img = cv2.resize(img, (TARGET_W, TARGET_H), interpolation=cv2.INTER_AREA)
            cv2.imwrite(out_path, img, [cv2.IMWRITE_JPEG_QUALITY, 90])
            os.remove(tmp_path)

            h, w = img.shape[:2]
            size_kb = os.path.getsize(out_path) / 1024
            print(f"OK ({w}x{h}, {size_kb:.0f} KB)")
            success += 1
        except Exception as e:
            print(f"FAILED ({e})")

    print(f"\nDone. {success}/{len(COCO_SAMPLES)} frames saved to {OUT_DIR}/")


def capture_webcam():
    """Original webcam capture mode."""
    import time

    NUM_FRAMES = 20
    DELAY_BEFORE = 3
    INTERVAL = 0.5

    os.makedirs(OUT_DIR, exist_ok=True)

    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("ERROR: cannot open /dev/video0")
        sys.exit(1)

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


if __name__ == "__main__":
    if "--webcam" in sys.argv:
        capture_webcam()
    else:
        download_coco_samples()
