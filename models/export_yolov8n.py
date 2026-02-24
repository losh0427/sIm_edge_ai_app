#!/usr/bin/env python3
"""
Export YOLOv8n to float32 TFLite for the edge AI pipeline.

Requirements:
    pip install ultralytics

Usage:
    python models/export_yolov8n.py            # 640x640 (default)
    python models/export_yolov8n.py --imgsz 320  # smaller / faster

The exported file is placed at models/yolov8n_float32.tflite.
"""

import argparse
import shutil
import sys
from pathlib import Path

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--imgsz", type=int, default=640,
                        help="Model input size (must be square, e.g. 320 or 640)")
    args = parser.parse_args()

    try:
        from ultralytics import YOLO
    except ImportError:
        print("ERROR: ultralytics not found.  Run:  pip install ultralytics", file=sys.stderr)
        sys.exit(1)

    print(f"Exporting YOLOv8n → float32 TFLite  imgsz={args.imgsz} ...")
    model = YOLO("yolov8n.pt")
    # nms=False keeps raw output [1, 84, num_anchors] which our C++ parser expects
    model.export(format="tflite", imgsz=args.imgsz, nms=False)

    # ultralytics saves to yolov8n_saved_model/yolov8n_float32.tflite (relative to CWD)
    candidates = [
        Path(f"yolov8n_saved_model/yolov8n_float32.tflite"),
        Path(f"yolov8n_saved_model/yolov8n_float32.tflite"),
    ]
    src = None
    for c in candidates:
        if c.exists():
            src = c
            break

    if src is None:
        # Try a wider search
        found = list(Path(".").rglob("yolov8n_float32.tflite"))
        if found:
            src = found[0]

    if src is None:
        print("Could not find exported .tflite file.  Check ultralytics output.", file=sys.stderr)
        sys.exit(1)

    dst = Path("models/yolov8n_float32.tflite")
    dst.parent.mkdir(exist_ok=True)
    shutil.copy(src, dst)
    size_mb = dst.stat().st_size / 1_048_576
    print(f"Saved: {dst}  ({size_mb:.1f} MB)")
    print()
    print("Next steps:")
    print("  1. Update docker-compose.yml:")
    print("       MODEL_PATH=/app/models/yolov8n_float32.tflite")
    print("       LABELS_PATH=/app/models/coco80_labels.txt")
    print("  2. docker compose up --build")

if __name__ == "__main__":
    main()
