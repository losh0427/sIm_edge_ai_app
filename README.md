# Edge AI Runtime

Real-time object detection on simulated IoT edge devices.
A C++ edge agent captures frames, runs TFLite inference, and streams results over gRPC to a Python server with a live Streamlit dashboard.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Device (C++17)                                         │
│                                                         │
│  ┌──────────┐    ┌──────────┐    ┌──────────────────┐   │
│  │  recv     │───▶│  infer   │───▶│  upload          │   │
│  │  thread   │    │  thread  │    │  thread          │   │
│  │           │    │          │    │                  │   │
│  │ HAL Frame │    │ TFLite   │    │ JPEG encode      │   │
│  │ Source    │    │ + NMS    │    │ + gRPC send      │   │
│  └──────────┘    └──────────┘    └────────┬─────────┘   │
│       RingBuffer A      RingBuffer B      │             │
│       (POSIX sem)       (POSIX sem)       │             │
└───────────────────────────────────────────┼─────────────┘
                                            │ gRPC (TCP :50051)
┌───────────────────────────────────────────┼─────────────┐
│  Server (Python)                          │             │
│                                           ▼             │
│  ┌──────────────────┐    ┌──────────────────────────┐   │
│  │  gRPC Receiver   │───▶│  Thread-safe Store       │   │
│  │  (background)    │    │  (mutex-guarded dict)    │   │
│  └──────────────────┘    └────────────┬─────────────┘   │
│                                       │                 │
│  ┌────────────────────────────────────▼─────────────┐   │
│  │  Streamlit Dashboard (:8501)                     │   │
│  │  • Live bounding-box overlay on thumbnails       │   │
│  │  • Per-device latency, FPS, detection count      │   │
│  │  • Multi-device grid layout                      │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### Key Design Decisions

| Decision | Rationale |
|---|---|
| **3-thread pipeline** (recv → infer → upload) | Decouples I/O from compute; each stage runs at its own pace |
| **POSIX SPSC RingBuffer** (semaphores + mutex) | Zero-malloc steady state, lock-free-like producer/consumer |
| **HAL pattern** (`IFrameSource` interface) | Compile-time backend switching: `FILE`, `OPENCV`, future `V4L2`/`SHM` |
| **Hand-written NMS** (`compute_iou` + `nms_filter`) | No framework dependency; IoU threshold runtime-configurable |
| **Runtime model auto-detect** | Same binary handles SSD MobileNet v2 and YOLOv8n based on tensor shapes |
| **gRPC with protobuf** | Language-agnostic, efficient serialization, natural for microservices |
| **No exceptions, no Boost** | C-style error handling (`errno` + `fprintf`), POSIX-first for embedded affinity |

## Tech Stack

| Component | Technology |
|---|---|
| Device agent | C++17, CMake, TFLite v2.14.0 (built from source), OpenCV, gRPC C++ |
| Server | Python 3.11, Streamlit, gRPC, OpenCV |
| Serialization | Protocol Buffers 3 |
| Infrastructure | Docker Compose, multi-stage builds |
| Camera (WSL2) | usbipd-win + custom kernel (V4L2/UVC) |

## Supported Models

| Model | Input | Quantization | NMS | Use Case |
|---|---|---|---|---|
| SSD MobileNet v2 | 300x300 uint8 | INT8 | Built-in (model outputs post-NMS) | Fast, lightweight |
| YOLOv8n | 640x640 float32 | None (float32) | Hand-written C++ | Higher mAP, ~10-15% better |

Both models are auto-detected at runtime based on output tensor shape — no recompilation needed.

## Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| Docker & Docker Compose | v20+ / v2+ | Both machines |
| Python | 3.9+ | For test tools and model export |
| Git | — | Clone the repo |
| WSL2 (Device on Windows) | — | Only if running device on Windows |
| usbipd-win (optional) | 5.3.0+ | Only for USB webcam in WSL2 |

## Quick Start — Single Machine

Everything runs on one machine via Docker Compose.

```bash
# 1. Clone & enter repo
git clone <repo-url> && cd sIm_edge_ai_app

# 2. Download model
bash models/download_model.sh

# 3. Generate test frames (dummy JPEGs for FILE mode)
pip install opencv-python numpy
python gen_test_frames.py

# 4. Build & run both containers
docker compose up --build

# 5. Open dashboard
#    http://localhost:8501
```

### Use YOLOv8n instead of SSD

```bash
pip install ultralytics
python models/export_yolov8n.py

MODEL_PATH=/app/models/yolov8n_float32.tflite \
LABELS_PATH=/app/models/coco80_labels.txt \
docker compose up --build
```

## Two-Machine Deployment

```
Machine A (Server)  e.g. 192.168.0.195
  └─ docker compose up server
       ├─ :50051  gRPC receiver
       └─ :8501   Streamlit dashboard

Machine B (Device)  e.g. 192.168.0.60  (WSL2)
  └─ docker compose up device --no-deps
       ├─ /dev/video0  webcam (OPENCV backend)
       └─ → Machine A:50051  gRPC upstream
```

### Machine A — Server Setup

```bash
# 1. Download model (first time only)
bash models/download_model.sh

# 2. Start server
docker compose up server --build     # first time
docker compose up server             # subsequent runs
```

Verify:
- gRPC: `nc -zv localhost 50051` → succeeded
- UI: `http://<Machine_A_IP>:8501`

### Machine A — Network Setup (Windows + WSL2)

Docker runs inside WSL2, so external devices need port forwarding through Windows.

**Firewall (one-time, persists across reboots):**
```powershell
# Admin PowerShell
New-NetFirewallRule -DisplayName "WSL2 gRPC 50051" `
  -Direction Inbound -Action Allow -Protocol TCP -LocalPort 50051

New-NetFirewallRule -DisplayName "WSL2 Streamlit 8501" `
  -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8501
```

**Port proxy (check after each reboot — WSL2 IP may change):**
```powershell
# Check current rules
netsh interface portproxy show v4tov4

# Check WSL IP
wsl -- hostname -I

# If IP changed, delete old and add new:
netsh interface portproxy delete v4tov4 listenport=50051 listenaddress=0.0.0.0
netsh interface portproxy delete v4tov4 listenport=8501  listenaddress=0.0.0.0

netsh interface portproxy add v4tov4 listenport=50051 listenaddress=0.0.0.0 connectport=50051 connectaddress=<WSL_IP>
netsh interface portproxy add v4tov4 listenport=8501  listenaddress=0.0.0.0 connectport=8501  connectaddress=<WSL_IP>
```

### Machine B — Device Setup

**1. USB webcam passthrough (WSL2, each reboot):**

```powershell
# Admin PowerShell on Machine B
usbipd list                          # find camera BUS-ID
usbipd bind --busid <BUS-ID>
usbipd attach --wsl --busid <BUS-ID>
```

```bash
# WSL
lsusb                                # confirm camera visible
ls /dev/video*                       # should see /dev/video0
sudo chmod 666 /dev/video0 /dev/video1
```

> First-time WSL2 camera setup requires a custom kernel with V4L2/UVC support.
> See [camera_research/WSL2_USB_CAMERA_GUIDE.md](camera_research/WSL2_USB_CAMERA_GUIDE.md) for the full guide.

**2. Build device image (first time ~15 min, TFLite compile):**

```bash
docker build \
  --build-arg HAL_CAMERA_BACKEND=OPENCV \
  -t sim_edge_ai_app-device \
  -f device/Dockerfile .
```

**3. Run device (SSD MobileNet):**

```bash
SERVER_ADDR=<Machine_A_IP>:50051 \
HAL_CAMERA_BACKEND=OPENCV \
docker compose up device --no-deps --build
```

**3b. Run device (YOLOv8n):**

```bash
SERVER_ADDR=<Machine_A_IP>:50051 \
HAL_CAMERA_BACKEND=OPENCV \
MODEL_PATH=/app/models/yolov8n_float32.tflite \
LABELS_PATH=/app/models/coco80_labels.txt \
docker compose up device --no-deps --build
```

**Verify** — device log should show:
```
gRPC connected to <Machine_A_IP>:50051
[upload] Frame 1 | 7.5 FPS | 124.4 ms | 1 detections
```

### Connectivity Test

From Device machine, verify network path to Server:

```powershell
# PowerShell on Machine B
Test-NetConnection <Machine_A_IP> -Port 50051
# TcpTestSucceeded : True
```

## Test Server Independently

No device or camera needed — use the Python fake client:

```bash
# Terminal 1: Start server
docker compose up server

# Terminal 2: Send fake detections
pip install grpcio grpcio-tools opencv-python numpy protobuf
python test_grpc_client.py localhost:50051
```

## Environment Variables

All runtime-configurable, no recompilation needed.

| Variable | Default | Description |
|---|---|---|
| `SERVER_ADDR` | `server:50051` | gRPC server address (use IP for two-machine) |
| `EDGE_ID` | `edge-1` | Device identifier (unique per device) |
| `MODEL_PATH` | `/app/models/ssd_mobilenet_v2.tflite` | TFLite model path inside container |
| `LABELS_PATH` | `/app/models/coco_labels.txt` | Labels file (`coco_labels.txt` for SSD, `coco80_labels.txt` for YOLOv8) |
| `INPUT_DIR` | `/app/data/test_frames` | JPEG directory (FILE backend only) |
| `CONF_THRESH` | `0.4` | Detection confidence threshold |
| `IOU_THRESH` | `0.45` | NMS IoU threshold (higher = more overlapping boxes) |
| `CAM_INDEX` | `0` | Camera device index (`/dev/videoN`, OPENCV backend) |
| `HAL_CAMERA_BACKEND` | `FILE` | Compile-time HAL: `FILE` or `OPENCV` |

## Project Structure

```
├── proto/edge_ai.proto              # gRPC service contract
├── docker-compose.yml               # Orchestration (server + device)
│
├── device/
│   ├── Dockerfile                   # Multi-stage: TFLite compile → minimal runtime
│   ├── CMakeLists.txt               # HAL backend selection via -DHAL_CAMERA_BACKEND
│   ├── hal/
│   │   ├── i_frame_source.h         # IFrameSource interface
│   │   ├── file_frame_source.cpp    # FILE backend (reads JPEGs from directory)
│   │   └── opencv_frame_source.cpp  # OPENCV backend (V4L2 webcam, MJPEG)
│   └── src/
│       ├── main.cpp                 # Entry point, env var parsing, pipeline init
│       ├── pipeline.cpp             # 3-thread orchestration (recv/infer/upload)
│       ├── ring_buffer.h            # POSIX SPSC queue (semaphores + mutex)
│       ├── inference.cpp            # TFLite runner + model auto-detection
│       ├── preprocess.cpp           # Resize + normalize for model input
│       ├── postprocess.cpp          # Hand-written NMS (compute_iou + nms_filter)
│       └── grpc_client.cpp          # gRPC client (DetectionFrame sender)
│
├── server/
│   ├── Dockerfile                   # Generates proto Python stubs at build time
│   ├── entrypoint.py                # Starts gRPC server thread, then Streamlit
│   └── app/
│       ├── main.py                  # Streamlit dashboard (live bbox overlay)
│       ├── grpc_server.py           # EdgeServicer (ReportDetection, ReportStats)
│       └── state.py                 # Thread-safe Store (mutex-guarded dict)
│
├── models/
│   ├── download_model.sh            # Download SSD MobileNet v2 INT8
│   ├── export_yolov8n.py            # Export YOLOv8n to float32 TFLite
│   ├── coco_labels.txt              # 91-class COCO labels (SSD)
│   └── coco80_labels.txt            # 80-class COCO labels (YOLOv8)
│
├── data/test_frames/                # Test JPEGs (generated, not in git)
├── test_grpc_client.py              # Python fake client for server testing
├── gen_test_frames.py               # Generate dummy test images
├── RUNBOOK.md                       # Two-machine deployment runbook
└── camera_research/
    └── WSL2_USB_CAMERA_GUIDE.md     # WSL2 USB camera setup (custom kernel + usbipd)
```

## gRPC API

Defined in [`proto/edge_ai.proto`](proto/edge_ai.proto):

```protobuf
service EdgeService {
    rpc ReportDetection (DetectionFrame) returns (ServerResponse);
    rpc ReportStats     (EdgeStats)      returns (ServerResponse);
}
```

**DetectionFrame** — sent per frame from device to server:
- `edge_id` — device identifier
- `boxes[]` — bounding boxes with normalized coords `[0,1]`, class ID, label, confidence
- `thumbnail_jpeg` — JPEG-encoded frame for dashboard visualization
- `inference_latency_ms` — end-to-end inference time
- `hal_backend` — which HAL backend produced the frame

**EdgeStats** — periodic device health report:
- `avg_latency_ms`, `p99_latency_ms`, `fps`
- `class_distribution` — detection count per class

## Build Times

| What changed | Rebuild time |
|---|---|
| First build (TFLite from source) | ~15 min |
| `device/src/` (C++ code) | ~3 min |
| `models/` or `data/` | ~10 sec |
| Server (Python) | ~2 min first, ~10 sec after |

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `mmap failed with error '22'` | Model file missing or 0 bytes | `bash models/download_model.sh` then rebuild |
| `gRPC connected` then disconnect | Server not running or firewall blocking | Start server first; check firewall rules |
| `/dev/video0: No such file` | USB camera not attached to WSL2 | Run `usbipd attach --wsl --busid <ID>` |
| `select() timeout` on camera | YUYV bandwidth too high over usbipd | Already handled — code forces MJPEG format |
| Streamlit shows 0 frames | Device connecting to wrong server | Check `SERVER_ADDR` and use `--no-deps` |
| Device starts local server | `depends_on` pulls in server service | Use `docker compose up device --no-deps` |
| Port proxy IP mismatch | WSL2 IP changed after reboot | Update `netsh interface portproxy` rules |

## Roadmap

- [x] MVP: single-threaded, FileFrameSource, single machine Docker
- [x] Multi-threaded pipeline (3 pthreads + RingBuffer)
- [x] Webcam support (OpenCVFrameSource + compile-time HAL)
- [x] YOLOv8n TFLite + runtime model auto-detect
- [x] NMS parameters runtime-configurable (CONF_THRESH / IOU_THRESH)
- [x] Two-machine deployment (device + server on separate hosts)
- [ ] Prometheus + Grafana metrics (replace/supplement Streamlit)
- [ ] Kernel module (`ml_stats.ko`), bpftrace, QEMU ARM64
