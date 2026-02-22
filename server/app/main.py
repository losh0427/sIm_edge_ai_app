import streamlit as st
import numpy as np
import cv2
from PIL import Image
import io
import time
import sys
import os

# Add parent dir to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from app.state import store
from app.grpc_server import start_grpc_server

# Start gRPC server once
if "grpc_started" not in st.session_state:
    start_grpc_server(port=50051)
    st.session_state.grpc_started = True

st.set_page_config(page_title="Edge AI Runtime", layout="wide")
st.title("🎯 Edge AI Runtime — Detection Dashboard")

# Auto-refresh
placeholder = st.empty()

COLORS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0),
           (255, 0, 255), (0, 255, 255), (128, 0, 0), (0, 128, 0)]

COCO_LABELS = []
labels_path = "/app/models/coco_labels.txt"
if os.path.exists(labels_path):
    with open(labels_path) as f:
        COCO_LABELS = [l.strip() for l in f if l.strip()]

def draw_boxes(jpeg_bytes, boxes, orig_w, orig_h):
    """Draw bounding boxes on thumbnail image."""
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

        class_id = box.get("class_id", -1)
        label = COCO_LABELS[class_id] if 0 <= class_id < len(COCO_LABELS) else f"cls_{class_id}"
        conf = box.get("confidence", 0)
        text = f"{label} {conf:.0%}"
        cv2.putText(img_np, text, (x0, max(y0 - 5, 10)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.4, color, 1)

    return img_np

# Main display loop
while True:
    latest = store.get_latest()
    total_frames = store.get_frame_count()

    with placeholder.container():
        if not latest:
            st.info("Waiting for edge devices to connect...")
            st.metric("Total Frames Received", total_frames)
        else:
            cols = st.columns(min(len(latest), 3))
            for i, (edge_id, result) in enumerate(latest.items()):
                with cols[i % len(cols)]:
                    st.subheader(f"📷 {edge_id}")

                    # Display annotated image
                    if result.thumbnail_jpeg:
                        img = draw_boxes(result.thumbnail_jpeg, result.boxes,
                                        result.orig_width, result.orig_height)
                        st.image(img, caption=f"Frame #{result.frame_number}", use_container_width=True)

                    # Stats
                    c1, c2, c3 = st.columns(3)
                    c1.metric("Latency", f"{result.latency_ms:.1f} ms")
                    c2.metric("Detections", len(result.boxes))
                    c3.metric("Frame", result.frame_number)

                    # Detection list
                    if result.boxes:
                        for box in result.boxes:
                            cid = box.get("class_id", -1)
                            lbl = COCO_LABELS[cid] if 0 <= cid < len(COCO_LABELS) else f"cls_{cid}"
                            st.text(f"  {lbl}: {box['confidence']:.0%}")

                    st.caption(f"HAL: {result.hal_backend}")

        st.metric("Total Frames", total_frames)

    time.sleep(0.5)
