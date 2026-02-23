# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is
IoT edge device simulation: C++ agent reads images → TFLite MobileNet-SSD object detection → gRPC → Python server with Streamlit UI.
Portfolio project targeting Google Taiwan new grad SWE (Home/Nest/ChromeOS/Cloud).

## Architecture (MVP V1 - Current)
- **Single machine**, two Docker containers on same `edge-net` bridge network
- `device`: C++ agent — FileFrameSource reads test JPEGs → preprocess (resize) → TFLite INT8 inference → hand-written NMS → gRPC client sends DetectionFrame to server
- `server`: Python — gRPC receiver stores results → Streamlit UI draws bounding boxes on thumbnails, shows live stats
- Both containers built from repo root context via `docker compose up --build`

## Tech Stack
- **Device**: C++17, CMake, TFLite (built from source v2.14.0), OpenCV, gRPC C++, POSIX APIs
- **Server**: Python 3.11, Streamlit, gRPC, OpenCV
- **Infra**: Docker Compose, protobuf

## Key Design Decisions
- HAL pattern: `IFrameSource` interface. Currently `FileFrameSource` (reads JPEGs). Future: `V4L2FrameSource`, `OpenCVFrameSource`, `ShmFrameSource`
- Compile-time HAL switching via CMake flag: `cmake -DHAL_CAMERA_BACKEND=FILE|V4L2|OPENCV|SHM ..`
- Hand-written NMS (`compute_iou` + `nms_filter`) — not using any framework's built-in postprocess
- 3-thread pipeline: recv → RingBuffer A → infer → RingBuffer B → upload (POSIX semaphores + pthread_mutex, SPSC, 4 slots each, ~8.4 MB total)
- Zero malloc in steady state for recv/infer threads; upload thread has small JPEG encode + protobuf allocs (I/O-bound, acceptable)
- TFLite C++ API (not C API), model is SSD MobileNet v2 INT8, input 300x300 uint8
- `state.py` `Store` is the single shared object between the gRPC thread and Streamlit's polling loop; all access is mutex-guarded
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
  src/                       # main.cpp, pipeline, ring_buffer, preprocess, inference, postprocess(NMS), grpc_client, labels
server/
  Dockerfile                 # Generates proto Python stubs at build time
  app/main.py                # Streamlit UI + gRPC server startup (starts gRPC in background thread)
  app/grpc_server.py         # EdgeServicer (ReportDetection, ReportStats)
  app/state.py               # Thread-safe Store shared between gRPC and Streamlit
models/                      # .tflite model + coco_labels.txt (model not in git)
data/test_frames/            # Test JPEGs (not in git)
docker-compose.yml           # server(:8501,:50051) + device, edge-net bridge
test_grpc_client.py          # Python fake client to test server independently
gen_test_frames.py           # Generate dummy test images into data/test_frames/
```

## Build & Run
```bash
bash models/download_model.sh          # Download SSD MobileNet v2 INT8 .tflite
pip install opencv-python numpy
python gen_test_frames.py              # Populate data/test_frames/
docker compose up --build              # Build + run both containers (device build ~15min first time)
# Server UI: http://localhost:8501
```

## Test Server Independently
```bash
docker compose up server
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
| `MODEL_PATH` | `/app/models/ssd_mobilenet_v2.tflite` | TFLite model path |
| `LABELS_PATH` | `/app/models/coco_labels.txt` | COCO labels file |
| `INPUT_DIR` | `/app/data/test_frames` | Directory of input JPEG frames |
| `CONF_THRESH` | `0.4` | Detection confidence threshold |
| `CAM_INDEX` | `0` | Camera device index for OPENCV backend (`/dev/videoN`) |
| `HAL_CAMERA_BACKEND` | `FILE` | Compile-time HAL switch: `FILE` or `OPENCV` |

## Current Status
- All source files written, not yet tested end-to-end
- Server likely works first (simpler Python stack)
- Device Docker build will be slow first time (TFLite source compile ~15min)
- May need CMake/linking fixes for gRPC C++ on Ubuntu 22.04

## Roadmap (in order)
1. ✅ MVP code written (single-threaded, FileFrameSource, single machine Docker)
2. Get server running and verified with test_grpc_client.py
3. Get device Docker build passing (TFLite + gRPC linking)
4. End-to-end: device → server → see detections in Streamlit
5. ✅ Multi-threaded pipeline (3 pthreads + RingBuffer with POSIX semaphores)
6. ✅ Webcam support (OpenCVFrameSource + compile-time HAL switching)
7. Two-machine deployment (device on laptop B, server on laptop A)
8. Kernel module (ml_stats.ko), bpftrace, QEMU ARM64 (bonus items)
9. Prometheus + Grafana (replaces/supplements Streamlit metrics)
