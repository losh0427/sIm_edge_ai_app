"""Pure drawing utilities — no framework dependency, only numpy/cv2/PIL."""

import io
import os
import numpy as np
import cv2
from PIL import Image

COLORS = [
    (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0),
    (255, 0, 255), (0, 255, 255), (128, 0, 0), (0, 128, 0),
]

_labels_cache: dict[str, list[str]] = {}


def load_labels(path: str) -> list[str]:
    if path in _labels_cache:
        return _labels_cache[path]
    labels: list[str] = []
    if os.path.exists(path):
        with open(path) as f:
            labels = [line.strip() for line in f if line.strip()]
    _labels_cache[path] = labels
    return labels


def get_label(labels: list[str], class_id: int) -> str:
    if 0 <= class_id < len(labels):
        return labels[class_id]
    return f"cls_{class_id}"


def draw_boxes(jpeg_bytes: bytes, boxes: list[dict], labels: list[str]) -> np.ndarray:
    """Draw bounding boxes on a JPEG image, return numpy array (RGB)."""
    img = Image.open(io.BytesIO(jpeg_bytes))
    img_np = np.array(img)
    h, w = img_np.shape[:2]

    for i, box in enumerate(boxes):
        color = COLORS[i % len(COLORS)]
        x0 = int(box["x_min"] * w)
        y0 = int(box["y_min"] * h)
        x1 = int(box["x_max"] * w)
        y1 = int(box["y_max"] * h)
        cv2.rectangle(img_np, (x0, y0), (x1, y1), color, 2)

        label = get_label(labels, box.get("class_id", -1))
        conf = box.get("confidence", 0)
        text = f"{label} {conf:.0%}"
        cv2.putText(img_np, text, (x0, max(y0 - 5, 10)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.4, color, 1)

    return img_np


def render_annotated_jpeg(jpeg_bytes: bytes, boxes: list[dict],
                          labels: list[str], quality: int = 85) -> bytes:
    """Draw boxes and return JPEG bytes (for FastAPI endpoint)."""
    img_np = draw_boxes(jpeg_bytes, boxes, labels)
    img_bgr = cv2.cvtColor(img_np, cv2.COLOR_RGB2BGR)
    _, buf = cv2.imencode(".jpg", img_bgr, [cv2.IMWRITE_JPEG_QUALITY, quality])
    return buf.tobytes()
