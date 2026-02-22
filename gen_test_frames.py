#!/usr/bin/env python3
"""Generate test images for FileFrameSource testing."""
import numpy as np
import cv2
import os

os.makedirs("data/test_frames", exist_ok=True)

for i in range(5):
    img = np.random.randint(40, 200, (480, 640, 3), dtype=np.uint8)
    # Draw some rectangles to simulate objects
    cv2.rectangle(img, (100 + i*20, 100), (300 + i*20, 350), (0, 255, 0), 3)
    cv2.rectangle(img, (350, 200 + i*10), (550, 400 + i*10), (255, 0, 0), 3)
    cv2.putText(img, f"Test Frame {i}", (200, 50), cv2.FONT_HERSHEY_SIMPLEX, 1, (255, 255, 255), 2)
    cv2.imwrite(f"data/test_frames/frame_{i:03d}.jpg", img)
    print(f"Generated frame_{i:03d}.jpg")

print("Done. Test frames in data/test_frames/")
