# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is
IoT edge device simulation: C++ agent reads images → TFLite object detection (SSD MobileNet v2 or YOLOv8n) → gRPC → Python server with NiceGUI dashboard (FastAPI + WebSocket).
Portfolio project targeting Google Taiwan new grad SWE (Home/Nest/ChromeOS/Cloud).

## Architecture (MVP V1 - Current)
- **Single machine**, two Docker containers on same `edge-net` bridge network
- `device`: C++ agent — FileFrameSource reads test JPEGs → preprocess (resize) → TFLite INT8 inference → hand-written NMS → gRPC client sends DetectionFrame to server
- `server`: Python — gRPC receiver stores results → NiceGUI dashboard draws bounding boxes on thumbnails, shows live stats + ECharts time-series
- Both containers built from repo root context via `docker compose up --build`

## Tech Stack
- **Device**: C++17, CMake, TFLite (built from source v2.14.0), OpenCV, gRPC C++, POSIX APIs
- **Server**: Python 3.11, NiceGUI (FastAPI/uvicorn), gRPC, OpenCV
- **Infra**: Docker Compose, protobuf

## Key Design Decisions
- HAL pattern: `IFrameSource` interface. Currently `FileFrameSource` (reads JPEGs). Future: `V4L2FrameSource`, `OpenCVFrameSource`, `ShmFrameSource`
- Compile-time HAL switching via CMake flag: `cmake -DHAL_CAMERA_BACKEND=FILE|V4L2|OPENCV|SHM ..`
- Hand-written NMS (`compute_iou` + `nms_filter`) — not using any framework's built-in postprocess; IoU threshold controlled via `IOU_THRESH` env var
- Pipeline Level system (0-4), compile-time switch via CMake `-DPIPELINE_LEVEL=N`:
  - **L0**: single-threaded blocking loop, `std::vector` heap alloc per frame
  - **L1**: 3x `std::thread` + `ThreadSafeQueue` (unbounded `std::queue` + mutex + cond_var)
  - **L2**: 3x `pthread_create` + `PosixQueue` (bounded `sem_t` + `pthread_mutex_t`, capacity=8)
  - **L3**: 3x `pthread` + `RingBuffer` + pre-allocated slots, `resize()` → `memcpy` (temporary alloc)
  - **L4** (default): 3x `pthread` + `RingBuffer` + `resize_into()` direct write (true zero-copy)
- Zero malloc in steady state for recv/infer threads (L4 only); upload thread has small JPEG encode + protobuf allocs (I/O-bound, acceptable)
- TFLite C++ API (not C API); auto-detects model type at runtime:
  - `SSD_MOBILENET`: 4 output tensors (post-NMS from model), uint8 input, 300x300
  - `YOLOV8`: 1 output tensor `[1, 4+num_classes, num_anchors]`, float32 input, 640x640; C++ NMS applied
- Pre-allocated input buffer sized for largest model (640x640x3 = 1.2 MB/slot)
- `state.py` `Store` is the single shared object between the gRPC thread and NiceGUI's timer callback; all access is mutex-guarded
- Proto stubs for Python are generated at Docker image build time (not committed to git); the server Dockerfile runs `grpc_tools.protoc` to emit `edge_ai_pb2.py` and `edge_ai_pb2_grpc.py` into `/app/server/`

## Code Style
- C++: snake_case, .cpp/.h, `#pragma once`, 4-space indent, no exceptions (errno + fprintf(stderr))
- No Boost. POSIX preferred over STL threading (pthread > std::thread) for later phases
- Zero malloc in steady state (pre-allocate everything at startup)
- Python: standard, minimal, functional

## Directory Layout
```
proto/edge_ai.proto          # gRPC contract (DetectionFrame, EdgeStats, EdgeService)
device/
  CMakeLists.txt             # TFLite from /tf source dir, gRPC via pkg-config
  Dockerfile                 # Multi-stage: builder (compile TFLite ~15min) → runtime
  hal/                       # IFrameSource interface + FileFrameSource
  src/                       # main.cpp, pipeline (L0-L4), ring_buffer, preprocess, inference, postprocess(NMS), grpc_client, labels
server/
  Dockerfile                 # Generates proto Python stubs at build time
  app/main.py                # NiceGUI dashboard (@ui.page + FastAPI endpoints)
  app/draw.py                # Pure drawing utilities (draw_boxes, render_annotated_jpeg)
  app/grpc_server.py         # EdgeServicer (ReportDetection, ReportStats)
  app/state.py               # Thread-safe Store shared between gRPC and NiceGUI
  entrypoint.py              # Starts gRPC server + ui.run()
models/                      # .tflite model + coco_labels.txt (model not in git)
data/test_frames/            # Test JPEGs (not in git)
docker-compose.yml           # server(:8501,:50051) + device, edge-net bridge
test_grpc_client.py          # Python fake client to test server independently
gen_test_frames.py           # Generate dummy test images into data/test_frames/
```

## Local Python Environment Setup

```bash
python3 -m venv .venv
source .venv/bin/activate          # activate once per shell session
pip install -r requirements-dev.txt
```
`.venv/` is already in `.gitignore`.

Includes: numpy, opencv-python, grpcio, grpcio-tools, protobuf, ultralytics (+ PyTorch ~1.5 GB first install).

## Build & Run
```bash
# Option A: SSD MobileNet v2 (default, INT8, fast build)
bash models/download_model.sh
pip install opencv-python numpy
python gen_test_frames.py
docker compose up --build
# Server UI: http://localhost:8501

# Option B: YOLOv8n float32 (higher mAP, ~10-15% better than SSD)
pip install ultralytics
python models/export_yolov8n.py        # Exports models/yolov8n_float32.tflite
MODEL_PATH=/app/models/yolov8n_float32.tflite \
LABELS_PATH=/app/models/coco80_labels.txt \
docker compose up --build
```

## Build with Different Pipeline Levels
```bash
# Level 0 (single-threaded, simplest)
PIPELINE_LEVEL=0 docker compose build device

# Level 2 (pthread + bounded queue)
PIPELINE_LEVEL=2 docker compose up --build device

# Default is Level 4 (full production)
docker compose up --build
```

## Two-Machine Deployment (webcam + YOLOv8n)
```bash
# Machine A (server): run on the server machine
docker compose up server --build
# Dashboard: http://<server-ip>:8501

# Machine B (device): run on the device machine with webcam
SERVER_ADDR=<server-ip>:50051 \
HAL_CAMERA_BACKEND=OPENCV \
MODEL_PATH=/app/models/yolov8n_float32.tflite \
LABELS_PATH=/app/models/coco80_labels.txt \
docker compose up device --build
```

## Test Server Independently
```bash
docker compose up server --build
pip install grpcio grpcio-tools opencv-python numpy protobuf
python test_grpc_client.py localhost:50051
```

## Regenerate Proto Stubs (local dev, outside Docker)
```bash
# Python
python -m grpc_tools.protoc -I proto --python_out=server --grpc_python_out=server proto/edge_ai.proto

# C++ (handled automatically by CMake's add_custom_command on build)
```

## Device Environment Variables
| Variable | Default | Description |
|---|---|---|
| `SERVER_ADDR` | `server:50051` | gRPC server address |
| `EDGE_ID` | `edge-1` | Device identifier sent in each frame |
| `MODEL_PATH` | `/app/models/ssd_mobilenet_v2.tflite` | TFLite model path (SSD or YOLOv8) |
| `LABELS_PATH` | `/app/models/coco_labels.txt` | Labels file (`coco_labels.txt` for SSD, `coco80_labels.txt` for YOLOv8) |
| `INPUT_DIR` | `/app/data/test_frames` | Directory of input JPEG frames |
| `CONF_THRESH` | `0.4` | Detection confidence threshold (pre-filter + NMS score gate) |
| `IOU_THRESH` | `0.45` | NMS IoU threshold (higher = more overlapping boxes kept) |
| `CAM_INDEX` | `0` | Camera device index for OPENCV backend (`/dev/videoN`) |
| `HAL_CAMERA_BACKEND` | `FILE` | Compile-time HAL switch: `FILE` or `OPENCV` |
| `PIPELINE_LEVEL` | `4` | Compile-time pipeline level: 0 (single-thread) to 4 (full production) |

## Current Status
- Server + Device 已驗證 end-to-end（webcam → inference → gRPC → dashboard）
- Two-machine deployment 完成（RUNBOOK.md）
- Pipeline Level 0-4 benchmark 完成，有完整分析報告

## Roadmap

### Completed
1. ✅ MVP code written (single-threaded, FileFrameSource, single machine Docker)
2. ✅ Server running + test_grpc_client.py verified
3. ✅ Device Docker build passing (TFLite + gRPC linking)
4. ✅ End-to-end: device → server → NiceGUI dashboard
5. ✅ Multi-threaded pipeline (3 pthreads + RingBuffer with POSIX semaphores)
6. ✅ Webcam support (OpenCVFrameSource + compile-time HAL switching)
7. ✅ YOLOv8n TFLite support + runtime model auto-detect
8. ✅ NMS parameters (CONF_THRESH / IOU_THRESH) fully runtime-configurable
9. ✅ Two-machine deployment (device on laptop B, server on laptop A, RUNBOOK.md)
10. ✅ NiceGUI dashboard (FastAPI + WebSocket, ECharts, dark mode, device status)
11. ✅ Pipeline Levels 0-4 (progressive optimization showcase, compile-time switch, benchmark analysis)

### Next — Observability（~1-2 天）
12. Prometheus + Grafana
    - FastAPI `/metrics` endpoint (inference latency, FPS, detection count)
    - Docker Compose 加 prometheus + grafana 容器
    - Grafana dashboard 預設 JSON import
    - 和 NiceGUI dashboard 互補：NiceGUI = 即時操作，Grafana = 歷史趨勢

### Next — ML Optimization（~0.5 天）
13. YOLOv8n INT8 量化
    - 目前 inference ~78ms 是絕對瓶頸（佔 pipeline 81%）
    - INT8 量化預期降到 30-40ms，直接提升 FPS 從 12.8 → ~25
    - `models/export_yolov8n.py` 加 INT8 export path

### Future — Systems（偏 Home/Nest/ChromeOS，高區分度）
14. Kernel module (ml_stats.ko)
    - /proc/ml_stats 暴露 inference latency histogram
    - 展示 Linux kernel API、proc filesystem、module lifecycle
15. bpftrace observability
    - 不修改 user-space code，用 eBPF uprobe 追蹤 TFLite::Invoke latency
    - 搭配 kernel module 展示「不同層級的 observability」
16. QEMU ARM64 cross-compile
    - aarch64 cross-build + QEMU user-mode 執行
    - 展示嵌入式部署能力，模擬 Nest Hub / Coral 場景

### Future — Infrastructure（偏 Cloud，中區分度）
17. CI/CD (GitHub Actions)
    - Docker build + test_grpc_client 自動跑
    - 每個 PR 自動驗證 server 啟動 + gRPC 通訊
18. k3s edge orchestration
    - 輕量 K8s，DaemonSet 部署多 device pod
    - liveness/readiness probe + auto-restart
    - 和 Prometheus 天然整合
    - 適合展示 cloud-native 能力，但和 Docker Compose 有功能重疊
19. ShmFrameSource (shared memory HAL)
    - IPC zero-copy shared memory frame source
    - 展示 POSIX shm_open / mmap，符合 ChromeOS camera pipeline 架構
20. Dashboard 多 device 支援
    - Store 已支援多 edge_id，UI 加 device selector / 分頁顯示
