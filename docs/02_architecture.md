# System Architecture

## Overview

Two Docker containers on the same `edge-net` bridge network (or across two physical machines):

```
┌─────────────────────┐         gRPC          ┌─────────────────────┐
│   Device (C++17)    │ ──────────────────── ▶ │   Server (Python)   │
│   edge_agent binary │    TCP :50051          │   NiceGUI :8501     │
└─────────────────────┘                        └─────────────────────┘
```

---

## Device Architecture

### HAL Layer (Hardware Abstraction)

```
           IFrameSource (interface)
           ├── FileFrameSource      — reads JPEGs from directory (testing/benchmark)
           └── OpenCVFrameSource    — V4L2 webcam via OpenCV VideoCapture (production)
```

Backend selected at **compile time** via CMake flag `HAL_CAMERA_BACKEND`. Generates `#define HAL_USE_FILE` or `#define HAL_USE_OPENCV` — the unused backend is never compiled.

### Pipeline Architecture (Level 4 — Production)

```
recv thread          infer thread         upload thread
┌──────────┐        ┌──────────┐        ┌──────────────┐
│ capture   │        │ TFLite   │        │ JPEG encode  │
│ + resize  │──RB_A─▶│ Invoke() │──RB_B─▶│ + gRPC send  │
│ into slot │        │ + NMS    │        │              │
└──────────┘        └──────────┘        └──────────────┘

RB_A: RingBuffer<InferSlot, 4>     — POSIX semaphore-based SPSC
RB_B: RingBuffer<UploadSlot, 4>    — POSIX semaphore-based SPSC
```

Three `pthread` threads in parallel:
1. **recv** — acquires frame from HAL, `resize_into()` writes directly into pre-allocated slot (zero heap alloc)
2. **infer** — TFLite `Invoke()` + hand-written NMS, copies results to UploadSlot
3. **upload** — JPEG thumbnail encode + gRPC send

RingBuffer provides natural **backpressure** via semaphore wait — prevents unbounded queue growth.

### Inference Engine

| Model | Input | Output | NMS |
|---|---|---|---|
| SSD MobileNet v2 | 300x300 uint8 | 4 tensors (post-NMS from model) | Built into model |
| YOLOv8n | 640x640 float32 | 1 tensor `[1, 84, 8400]` (raw anchors) | Hand-written C++ NMS |

Auto-detected at load time by output tensor count (4 → SSD, 1 → YOLOv8).

### Pre-Allocated Slots

`InferSlot` and `UploadSlot` are fixed-size structs with compile-time sized buffers (1.2 MB per slot for 640x640x3). Allocated once at startup — zero per-frame heap allocation in recv/infer threads. See `pipeline_slots.h`.

---

## Server Architecture

```
┌────────────────────────────────────────────────────┐
│  entrypoint.py                                     │
│                                                    │
│  ┌─────────────────┐    ┌───────────────────────┐  │
│  │ gRPC Server      │    │ Store (thread-safe)   │  │
│  │ (background      │───▶│ {edge_id: Detection}  │  │
│  │  thread :50051)  │    │ mutex + Event         │  │
│  └─────────────────┘    └───────────┬───────────┘  │
│                                     │              │
│  ┌──────────────────────────────────▼───────────┐  │
│  │ NiceGUI Dashboard (:8501)                    │  │
│  │  • FastAPI backend (REST + WebSocket)        │  │
│  │  • ui.timer(0.5s) polls Store for updates    │  │
│  │  • ECharts for FPS / latency time-series     │  │
│  │  • Annotated JPEG endpoint with bbox overlay │  │
│  └──────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────┘
```

- **`grpc_server.py`** — `ReportDetection` / `ReportStats` RPCs, writes into Store
- **`state.py`** — mutex-guarded dict keyed by `edge_id`, `threading.Event` for new frame notification
- **`main.py`** — NiceGUI dashboard with per-device cards, ECharts, annotated JPEG endpoint
- **`draw.py`** — OpenCV bounding box overlay utilities

Proto Python stubs generated at **Docker build time** (not committed to git).

---

## Compile-Time Configuration

| CMake Flag | Values | Effect |
|---|---|---|
| `HAL_CAMERA_BACKEND` | `FILE`, `OPENCV` | Selects IFrameSource implementation |
| `PIPELINE_LEVEL` | `0`-`4` | Selects pipeline implementation file |

Both passed via Docker Compose `build.args` in `docker-compose.yml`.

---

## Docker Build

- **Device** — multi-stage: builder compiles TFLite from source (~15 min, cached) + project binary → runtime copies binary + libs only
- **Server** — `python:3.11-slim`, pip install + `grpc_tools.protoc` generates stubs

---

## Network

**Single machine**: both containers on `edge-net` bridge, device connects to `server:50051`.

**Two machines**: server exposes ports via Windows port proxy (if WSL2), device uses `SERVER_ADDR=<server-ip>:50051`. Details in [RUNBOOK.md](../RUNBOOK.md).
