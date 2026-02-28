# Setup & Commands

## Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| Docker & Docker Compose | v20+ / v2+ | Required on both machines for two-machine setup |
| Python | 3.9+ | For test tools, model export, local dev |
| WSL2 + usbipd-win (optional) | 5.3.0+ | Only for USB webcam on Windows |

## Local Python Environment

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements-dev.txt
```

---

## Model Preparation

```bash
# Option A: SSD MobileNet v2 (INT8, ~4 MB)
bash models/download_model.sh

# Option B: YOLOv8n (float32, ~12 MB, higher accuracy)
pip install ultralytics && python models/export_yolov8n.py
```

## Test Frames

```bash
python gen_test_frames.py            # downloads 20 COCO images → data/test_frames/
python gen_test_frames.py --webcam   # or capture from webcam
```

---

## Single Machine

```bash
# SSD MobileNet (default)
docker compose up --build

# YOLOv8n
MODEL_PATH=/app/models/yolov8n_float32.tflite \
LABELS_PATH=/app/models/coco80_labels.txt \
docker compose up --build
```

Dashboard: http://localhost:8501

---

## Two-Machine Deployment

See [RUNBOOK.md](../RUNBOOK.md) for full setup including firewall, port proxy, and webcam passthrough.

```bash
# Machine A (server)
docker compose up server --build

# Machine B (device)
SERVER_ADDR=<Machine_A_IP>:50051 \
HAL_CAMERA_BACKEND=OPENCV \
docker compose up device --no-deps --build
```

---

## Pipeline Level Switching

```bash
PIPELINE_LEVEL=0 docker compose build device   # single-threaded
PIPELINE_LEVEL=2 docker compose build device   # pthread + bounded queue
docker compose up --build                       # Level 4 default (zero-copy)
```

Compile-time switch — requires rebuilding the device image.

---

## Benchmark

```bash
bash bench_cpp.sh       # C++ benchmark → benchmark_results/cpp_<timestamp>/
bash bench_python.sh    # Python benchmark → benchmark_results/python_<timestamp>/
```

---

## Test Server Independently

```bash
docker compose up server
python test_grpc_client.py localhost:50051   # sends 100 fake frames
```

---

## Environment Variables

| Variable | Default | Description |
|---|---|---|
| `SERVER_ADDR` | `server:50051` | gRPC server address |
| `EDGE_ID` | `edge-1` | Device identifier |
| `MODEL_PATH` | `/app/models/ssd_mobilenet_v2.tflite` | TFLite model path |
| `LABELS_PATH` | `/app/models/coco_labels.txt` | Labels file |
| `INPUT_DIR` | `/app/data/test_frames` | JPEG directory (FILE backend) |
| `CONF_THRESH` | `0.4` | Detection confidence threshold |
| `IOU_THRESH` | `0.45` | NMS IoU threshold |
| `CAM_INDEX` | `0` | Camera index (`/dev/videoN`) |
| `HAL_CAMERA_BACKEND` | `FILE` | Compile-time: `FILE` or `OPENCV` |
| `PIPELINE_LEVEL` | `4` | Compile-time: 0-4 |

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `mmap failed with error '22'` | Model missing — `bash models/download_model.sh` then rebuild |
| `gRPC connected` then disconnect | Start server first; check firewall |
| `/dev/video0: No such file` | `usbipd attach --wsl --busid <ID>` |
| `select() timeout` on camera | Code forces MJPEG — check USB connection |
| Dashboard shows 0 frames | Check `SERVER_ADDR`; use `--no-deps` for two-machine |
