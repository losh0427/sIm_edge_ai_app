# Setup & Commands

## Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| Docker & Docker Compose | v20+ / v2+ | Required on both machines for two-machine setup |
| Python | 3.9+ | For test tools, model export, local dev |
| Git | any | Clone the repo |
| WSL2 (optional) | — | Only if running device on Windows |
| usbipd-win (optional) | 5.3.0+ | Only for USB webcam passthrough to WSL2 |

## Local Python Environment

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements-dev.txt
```

The `.venv/` directory is already in `.gitignore`. This installs numpy, opencv-python, grpcio, grpcio-tools, protobuf, and ultralytics (for YOLOv8 export).

---

## Model Preparation

### Option A: SSD MobileNet v2 (INT8, fast build)

```bash
bash models/download_model.sh
```

Downloads `ssd_mobilenet_v2.tflite` (~4 MB) and `coco_labels.txt` (91 classes) into `models/`.

### Option B: YOLOv8n (float32, higher accuracy)

```bash
pip install ultralytics
python models/export_yolov8n.py
```

Exports `yolov8n_float32.tflite` (~12 MB) into `models/`. Uses `coco80_labels.txt` (80 classes, already in repo).

---

## Test Frame Generation

```bash
pip install opencv-python numpy
python gen_test_frames.py
```

Downloads 20 COCO val2017 sample images, resizes to 640x480, saves to `data/test_frames/`. These are used by `FileFrameSource` in FILE mode.

For webcam capture instead:
```bash
python gen_test_frames.py --webcam
```

---

## Single Machine (Docker Compose)

### SSD MobileNet v2 (default)

```bash
bash models/download_model.sh
python gen_test_frames.py
docker compose up --build
```

Dashboard: http://localhost:8501

### YOLOv8n

```bash
python models/export_yolov8n.py

MODEL_PATH=/app/models/yolov8n_float32.tflite \
LABELS_PATH=/app/models/coco80_labels.txt \
docker compose up --build
```

---

## Two-Machine Deployment

### Machine A (Server)

```bash
docker compose up server --build
```

Verify:
- gRPC: `nc -zv localhost 50051`
- Dashboard: `http://<Machine_A_IP>:8501`

### Machine A — Network (Windows + WSL2)

**Firewall (one-time):**
```powershell
# Admin PowerShell
New-NetFirewallRule -DisplayName "WSL2 gRPC 50051" `
  -Direction Inbound -Action Allow -Protocol TCP -LocalPort 50051

New-NetFirewallRule -DisplayName "WSL2 Dashboard 8501" `
  -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8501
```

**Port proxy (check after each reboot):**
```powershell
# Get WSL2 IP
wsl -- hostname -I

# Add port proxy
netsh interface portproxy add v4tov4 listenport=50051 listenaddress=0.0.0.0 connectport=50051 connectaddress=<WSL_IP>
netsh interface portproxy add v4tov4 listenport=8501  listenaddress=0.0.0.0 connectport=8501  connectaddress=<WSL_IP>

# Verify
netsh interface portproxy show v4tov4
```

### Machine B (Device) — USB Webcam

```powershell
# Admin PowerShell on Machine B
usbipd list                          # find camera BUS-ID
usbipd bind --busid <BUS-ID>
usbipd attach --wsl --busid <BUS-ID>
```

```bash
# WSL on Machine B
lsusb                                # confirm camera visible
ls /dev/video*                       # should see /dev/video0
sudo chmod 666 /dev/video0 /dev/video1
```

### Machine B (Device) — Run

**SSD MobileNet:**
```bash
SERVER_ADDR=<Machine_A_IP>:50051 \
HAL_CAMERA_BACKEND=OPENCV \
docker compose up device --no-deps --build
```

**YOLOv8n:**
```bash
SERVER_ADDR=<Machine_A_IP>:50051 \
HAL_CAMERA_BACKEND=OPENCV \
MODEL_PATH=/app/models/yolov8n_float32.tflite \
LABELS_PATH=/app/models/coco80_labels.txt \
docker compose up device --no-deps --build
```

### Connectivity Test

```powershell
# From Machine B PowerShell
Test-NetConnection <Machine_A_IP> -Port 50051
# TcpTestSucceeded : True
```

---

## Pipeline Level Switching

```bash
# Level 0 (single-threaded, simplest)
PIPELINE_LEVEL=0 docker compose build device

# Level 1 (std::thread + unbounded queue)
PIPELINE_LEVEL=1 docker compose build device

# Level 2 (pthread + bounded queue, capacity=8)
PIPELINE_LEVEL=2 docker compose build device

# Level 3 (RingBuffer + pre-allocated slots, resize+memcpy)
PIPELINE_LEVEL=3 docker compose build device

# Level 4 (zero-copy production — default)
docker compose up --build
```

Level is a **compile-time** switch — changing it requires rebuilding the device image.

---

## Benchmark Execution

### C++ Benchmark

```bash
bash bench_cpp.sh
# Runs until Ctrl+C, then collects results
# Output: benchmark_results/cpp_<timestamp>/
```

### Python Benchmark

```bash
bash bench_python.sh
# Output: benchmark_results/python_<timestamp>/
```

Both scripts:
1. Start the server container
2. Launch the device with `BENCH_CSV` env var for per-frame CSV logging
3. Collect `docker stats` every 2 seconds
4. On Ctrl+C: stop device, save logs, run analysis

Output files per run:
```
benchmark_results/<agent>_<timestamp>/
├── config.txt           # test parameters
├── <agent>_latency.csv  # per-frame timing (capture, preprocess, inference, NMS, thumbnail, gRPC, E2E)
├── <agent>_docker_stats.csv  # CPU%, memory, network I/O
└── <agent>_device.log   # container stderr
```

---

## Test Server Independently

No device or camera needed:

```bash
# Terminal 1
docker compose up server

# Terminal 2
pip install grpcio grpcio-tools opencv-python numpy protobuf
python test_grpc_client.py localhost:50051
```

Sends 100 fake detection frames with synthetic JPEG thumbnails.

---

## Regenerate Proto Stubs (Local Dev)

```bash
# Python stubs (for local testing outside Docker)
python -m grpc_tools.protoc \
  -I proto \
  --python_out=server \
  --grpc_python_out=server \
  proto/edge_ai.proto

# C++ stubs are generated automatically by CMake at build time
```

---

## Environment Variables Reference

| Variable | Default | Description |
|---|---|---|
| `SERVER_ADDR` | `server:50051` | gRPC server address (use IP for two-machine) |
| `EDGE_ID` | `edge-1` | Device identifier (unique per device) |
| `MODEL_PATH` | `/app/models/ssd_mobilenet_v2.tflite` | TFLite model path inside container |
| `LABELS_PATH` | `/app/models/coco_labels.txt` | Labels file (`coco_labels.txt` for SSD, `coco80_labels.txt` for YOLOv8) |
| `INPUT_DIR` | `/app/data/test_frames` | JPEG directory (FILE backend only) |
| `CONF_THRESH` | `0.4` | Detection confidence threshold |
| `IOU_THRESH` | `0.45` | NMS IoU threshold (higher = more overlapping boxes kept) |
| `CAM_INDEX` | `0` | Camera device index (`/dev/videoN`, OPENCV backend) |
| `HAL_CAMERA_BACKEND` | `FILE` | Compile-time HAL: `FILE` or `OPENCV` |
| `PIPELINE_LEVEL` | `4` | Compile-time pipeline level: 0-4 |
| `BENCH_CSV` | (empty) | Path for benchmark CSV output (enables logging) |
| `BENCH_WARMUP` | `100` | Frames to skip before recording benchmark data |

---

## Build Times

| What Changed | Rebuild Time |
|---|---|
| First build (TFLite from source) | ~15 min |
| `device/src/` (C++ code only) | ~3 min |
| `models/` or `data/` | ~10 sec |
| Server (Python, first time) | ~2 min |
| Server (subsequent) | ~10 sec |

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `mmap failed with error '22'` | Model file missing or 0 bytes | `bash models/download_model.sh` then rebuild |
| `gRPC connected` then disconnect | Server not running or firewall blocking | Start server first; check firewall rules |
| `/dev/video0: No such file` | USB camera not attached to WSL2 | Run `usbipd attach --wsl --busid <ID>` |
| `select() timeout` on camera | YUYV bandwidth too high over usbipd | Already handled — code forces MJPEG format |
| Dashboard shows 0 frames | Device connecting to wrong server | Check `SERVER_ADDR`; use `--no-deps` for two-machine |
| Device starts local server | `depends_on` pulls in server service | Use `docker compose up device --no-deps` |
| Port proxy IP mismatch | WSL2 IP changed after reboot | Update `netsh interface portproxy` rules |
| TFLite build takes forever | First build compiles TFLite from source | Normal (~15 min); Docker layer cache speeds subsequent builds |
| gRPC linking errors | Protobuf version mismatch | Ensure `libgrpc++-dev` and `protobuf-compiler` versions match |
