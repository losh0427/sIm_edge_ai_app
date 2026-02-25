#!/usr/bin/env python3
"""
Python baseline device agent — single-threaded equivalent of the C++ pipeline.

Pipeline: read frame → preprocess → TFLite inference → NMS → encode thumbnail → gRPC send

Used for benchmarking against the C++ implementation to quantify:
  - FPS throughput
  - Per-frame latency breakdown
  - Memory usage (RSS)
  - CPU utilisation
"""
import os
import sys
import glob
import time
import resource
import csv

import cv2
import numpy as np

# ---------------------------------------------------------------------------
# Proto stubs (generated at Docker build time, or locally via grpc_tools)
# ---------------------------------------------------------------------------
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import grpc
import edge_ai_pb2
import edge_ai_pb2_grpc

# ---------------------------------------------------------------------------
# Config from environment (mirrors C++ device env vars)
# ---------------------------------------------------------------------------
SERVER_ADDR = os.environ.get("SERVER_ADDR", "server:50051")
EDGE_ID     = os.environ.get("EDGE_ID", "py-edge-1")
MODEL_PATH  = os.environ.get("MODEL_PATH", "/app/models/ssd_mobilenet_v2.tflite")
LABELS_PATH = os.environ.get("LABELS_PATH", "/app/models/coco_labels.txt")
INPUT_DIR   = os.environ.get("INPUT_DIR", "/app/data/test_frames")
CONF_THRESH = float(os.environ.get("CONF_THRESH", "0.4"))
IOU_THRESH  = float(os.environ.get("IOU_THRESH", "0.45"))
HAL_BACKEND = os.environ.get("HAL_CAMERA_BACKEND", "FILE")
CAM_INDEX   = int(os.environ.get("CAM_INDEX", "0"))
THUMB_MAX_W = 480
THUMB_QUALITY = 80

# Benchmark config
BENCH_CSV     = os.environ.get("BENCH_CSV", "")       # set to path to enable CSV logging
BENCH_WARMUP  = int(os.environ.get("BENCH_WARMUP", "100"))  # frames to skip

# ---------------------------------------------------------------------------
# Load labels
# ---------------------------------------------------------------------------
def load_labels(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [line.strip() for line in f if line.strip()]

LABELS = load_labels(LABELS_PATH)

# ---------------------------------------------------------------------------
# Model loading (tflite-runtime)
# ---------------------------------------------------------------------------
try:
    import tflite_runtime.interpreter as tflite
except ImportError:
    from tensorflow import lite as tflite

def load_model(path):
    interpreter = tflite.Interpreter(model_path=path, num_threads=2)
    interpreter.allocate_tensors()

    inp = interpreter.get_input_details()[0]
    out_details = interpreter.get_output_details()

    input_h, input_w = inp["shape"][1], inp["shape"][2]
    is_float = (inp["dtype"] == np.float32)

    if len(out_details) == 4:
        model_type = "SSD_MOBILENET"
    elif len(out_details) == 1 and len(out_details[0]["shape"]) == 3:
        model_type = "YOLOV8"
    else:
        model_type = "SSD_MOBILENET"

    print(f"Model: {model_type}  input={input_w}x{input_h}  "
          f"float={is_float}  outputs={len(out_details)}", file=sys.stderr)

    return interpreter, inp, out_details, input_w, input_h, is_float, model_type

# ---------------------------------------------------------------------------
# Inference
# ---------------------------------------------------------------------------
def run_inference(interpreter, inp_detail, input_data):
    interpreter.set_tensor(inp_detail["index"], input_data)
    interpreter.invoke()

def parse_ssd_output(interpreter, out_details, conf_thresh):
    boxes   = interpreter.get_tensor(out_details[0]["index"])[0]
    classes = interpreter.get_tensor(out_details[1]["index"])[0]
    scores  = interpreter.get_tensor(out_details[2]["index"])[0]
    count   = int(interpreter.get_tensor(out_details[3]["index"])[0])

    detections = []
    for i in range(count):
        if scores[i] < conf_thresh:
            continue
        detections.append({
            "y_min": float(boxes[i][0]), "x_min": float(boxes[i][1]),
            "y_max": float(boxes[i][2]), "x_max": float(boxes[i][3]),
            "class_id": int(classes[i]),
            "confidence": float(scores[i]),
        })
    return detections

def parse_yolov8_output(interpreter, out_details, conf_thresh):
    out = interpreter.get_tensor(out_details[0]["index"])[0]
    feat_dim, num_anchors = out.shape
    num_classes = feat_dim - 4

    detections = []
    for a in range(num_anchors):
        scores = out[4:, a]
        best_class = int(np.argmax(scores))
        best_score = float(scores[best_class])
        if best_score < conf_thresh:
            continue
        cx, cy, w, h = out[0, a], out[1, a], out[2, a], out[3, a]
        detections.append({
            "x_min": float(cx - w * 0.5), "y_min": float(cy - h * 0.5),
            "x_max": float(cx + w * 0.5), "y_max": float(cy + h * 0.5),
            "class_id": best_class,
            "confidence": best_score,
        })
    return detections

# ---------------------------------------------------------------------------
# NMS (pure Python, equivalent to C++ postprocess::nms_filter)
# ---------------------------------------------------------------------------
def compute_iou(a, b):
    ix0 = max(a["x_min"], b["x_min"])
    iy0 = max(a["y_min"], b["y_min"])
    ix1 = min(a["x_max"], b["x_max"])
    iy1 = min(a["y_max"], b["y_max"])
    inter = max(0, ix1 - ix0) * max(0, iy1 - iy0)
    area_a = (a["x_max"] - a["x_min"]) * (a["y_max"] - a["y_min"])
    area_b = (b["x_max"] - b["x_min"]) * (b["y_max"] - b["y_min"])
    union = area_a + area_b - inter
    return inter / union if union > 0 else 0

def nms_filter(detections, iou_thresh, score_thresh):
    filtered = [d for d in detections if d["confidence"] >= score_thresh]
    filtered.sort(key=lambda d: d["confidence"], reverse=True)
    suppressed = [False] * len(filtered)
    results = []
    for i in range(len(filtered)):
        if suppressed[i]:
            continue
        results.append(filtered[i])
        for j in range(i + 1, len(filtered)):
            if not suppressed[j] and compute_iou(filtered[i], filtered[j]) > iou_thresh:
                suppressed[j] = True
    return results

# ---------------------------------------------------------------------------
# Thumbnail encoding (aspect-ratio preserving)
# ---------------------------------------------------------------------------
def encode_thumbnail(frame_bgr, max_w=THUMB_MAX_W, quality=THUMB_QUALITY):
    h, w = frame_bgr.shape[:2]
    if w > max_w:
        new_w = max_w
        new_h = int(new_w * h / w)
        resized = cv2.resize(frame_bgr, (new_w, new_h), interpolation=cv2.INTER_AREA)
    else:
        resized = frame_bgr
    _, buf = cv2.imencode(".jpg", resized, [cv2.IMWRITE_JPEG_QUALITY, quality])
    return buf.tobytes(), w, h

# ---------------------------------------------------------------------------
# Frame source
# ---------------------------------------------------------------------------
def file_frame_source(input_dir):
    patterns = ["*.jpg", "*.jpeg", "*.png"]
    files = []
    for p in patterns:
        files.extend(sorted(glob.glob(os.path.join(input_dir, p))))
    if not files:
        print(f"No images found in {input_dir}", file=sys.stderr)
        return
    while True:
        for path in files:
            frame = cv2.imread(path)
            if frame is not None:
                yield frame

def opencv_frame_source(cam_index):
    cap = cv2.VideoCapture(cam_index)
    if not cap.isOpened():
        print(f"Cannot open camera {cam_index}", file=sys.stderr)
        return
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(f"Camera opened: {w}x{h}", file=sys.stderr)
    while True:
        ret, frame = cap.read()
        if not ret:
            break
        yield frame
    cap.release()

# ---------------------------------------------------------------------------
# RSS helper
# ---------------------------------------------------------------------------
def get_rss_kb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

# ---------------------------------------------------------------------------
# Main pipeline (single-threaded, with benchmark instrumentation)
# ---------------------------------------------------------------------------
def main():
    print(f"Python device agent starting", file=sys.stderr)
    print(f"  SERVER_ADDR={SERVER_ADDR}", file=sys.stderr)
    print(f"  MODEL_PATH={MODEL_PATH}", file=sys.stderr)
    print(f"  HAL_BACKEND={HAL_BACKEND}", file=sys.stderr)

    interpreter, inp_detail, out_details, input_w, input_h, is_float, model_type = \
        load_model(MODEL_PATH)

    if HAL_BACKEND == "OPENCV":
        source = opencv_frame_source(CAM_INDEX)
    else:
        source = file_frame_source(INPUT_DIR)

    channel = grpc.insecure_channel(SERVER_ADDR)
    stub = edge_ai_pb2_grpc.EdgeServiceStub(channel)

    # Benchmark CSV writer
    csv_file = None
    csv_writer = None
    if BENCH_CSV:
        csv_file = open(BENCH_CSV, "w", newline="")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow([
            "frame", "capture_us", "preprocess_us", "inference_us",
            "nms_us", "thumbnail_us", "grpc_us", "e2e_us",
            "num_dets", "rss_kb",
        ])
        print(f"Benchmark CSV: {BENCH_CSV} (warmup={BENCH_WARMUP})",
              file=sys.stderr)

    hal_desc = f"PYTHON:{HAL_BACKEND}"
    frame_num = 0
    start_time = time.monotonic()

    for frame_bgr in source:
        frame_num += 1
        t_start = time.perf_counter_ns()

        # --- Capture (already done by generator, measure overhead) ---
        t_capture = time.perf_counter_ns()

        # --- Preprocess ---
        frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        resized = cv2.resize(frame_rgb, (input_w, input_h))
        if is_float:
            input_data = np.expand_dims(resized.astype(np.float32) / 255.0, axis=0)
        else:
            input_data = np.expand_dims(resized, axis=0)
        t_preprocess = time.perf_counter_ns()

        # --- Inference ---
        run_inference(interpreter, inp_detail, input_data)
        if model_type == "YOLOV8":
            raw_dets = parse_yolov8_output(interpreter, out_details, CONF_THRESH)
        else:
            raw_dets = parse_ssd_output(interpreter, out_details, CONF_THRESH)
        t_inference = time.perf_counter_ns()

        # --- NMS ---
        dets = nms_filter(raw_dets, IOU_THRESH, CONF_THRESH)
        t_nms = time.perf_counter_ns()

        # --- Encode thumbnail ---
        thumbnail_jpeg, orig_w, orig_h = encode_thumbnail(frame_bgr)
        t_thumbnail = time.perf_counter_ns()

        # --- gRPC send ---
        inference_ms = (t_inference - t_preprocess) / 1e6
        req = edge_ai_pb2.DetectionFrame(
            edge_id=EDGE_ID,
            timestamp_ms=int(time.time() * 1000),
            thumbnail_jpeg=thumbnail_jpeg,
            inference_latency_ms=inference_ms,
            frame_number=frame_num,
            hal_backend=hal_desc,
            original_width=orig_w,
            original_height=orig_h,
        )
        for d in dets:
            box = req.boxes.add()
            box.x_min = d["x_min"]
            box.y_min = d["y_min"]
            box.x_max = d["x_max"]
            box.y_max = d["y_max"]
            box.class_id = d["class_id"]
            box.confidence = d["confidence"]

        try:
            stub.ReportDetection(req)
        except grpc.RpcError as e:
            print(f"gRPC error: {e.code()}", file=sys.stderr, flush=True)
        t_grpc = time.perf_counter_ns()

        # --- Benchmark CSV ---
        e2e_us = (t_grpc - t_start) / 1000
        if csv_writer and frame_num > BENCH_WARMUP:
            csv_writer.writerow([
                frame_num,
                int((t_capture - t_start) / 1000),
                int((t_preprocess - t_capture) / 1000),
                int((t_inference - t_preprocess) / 1000),
                int((t_nms - t_inference) / 1000),
                int((t_thumbnail - t_nms) / 1000),
                int((t_grpc - t_thumbnail) / 1000),
                int(e2e_us),
                len(dets),
                get_rss_kb(),
            ])

        # --- Console log ---
        elapsed = time.monotonic() - start_time
        fps = frame_num / elapsed if elapsed > 0 else 0
        if frame_num % 10 == 1 or frame_num <= 5:
            print(f"[py-device] Frame {frame_num} | {fps:.1f} FPS | "
                  f"{inference_ms:.1f} ms | {len(dets)} dets | "
                  f"e2e {e2e_us/1000:.1f} ms",
                  file=sys.stderr, flush=True)

    if csv_file:
        csv_file.close()
        print(f"Benchmark CSV saved: {BENCH_CSV}", file=sys.stderr)

if __name__ == "__main__":
    main()
