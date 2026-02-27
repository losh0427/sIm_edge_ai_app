# Project Overview

> **One-liner**: IoT edge device simulation — C++ agent captures frames, runs TFLite object detection, and streams results over gRPC to a Python server with a live NiceGUI dashboard.

## System Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Device Container (C++17)                               │
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
│  Server Container (Python)                │             │
│                                           ▼             │
│  ┌──────────────────┐    ┌──────────────────────────┐   │
│  │  gRPC Receiver   │───▶│  Thread-safe Store       │   │
│  │  (background)    │    │  (mutex-guarded dict)    │   │
│  └──────────────────┘    └────────────┬─────────────┘   │
│                                       │                 │
│  ┌────────────────────────────────────▼─────────────┐   │
│  │  NiceGUI Dashboard (:8501)                       │   │
│  │  • Live bounding-box overlay on thumbnails       │   │
│  │  • Per-device latency, FPS, detection count      │   │
│  │  • ECharts time-series (FPS / latency)           │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

## Tech Stack

| Component | Technology |
|---|---|
| Device agent | C++17, CMake, TFLite v2.14.0 (built from source), OpenCV, gRPC C++ |
| Server | Python 3.11, NiceGUI (FastAPI/uvicorn + WebSocket), gRPC, OpenCV |
| Serialization | Protocol Buffers 3 |
| Infrastructure | Docker Compose, multi-stage builds |
| Models | SSD MobileNet v2 (INT8, 300x300) / YOLOv8n (float32, 640x640) |

## Core Highlights

- **5 pipeline levels (L0-L4)** — progressive optimization from single-threaded naive loop to production-grade zero-malloc 3-thread pipeline with POSIX semaphore-based RingBuffer. Each level isolates a specific systems concept (threading, bounded queues, backpressure, memory pre-allocation, zero-copy writes).

- **Hand-written NMS** — `compute_iou()` + `nms_filter()` implemented from scratch in C++; no framework dependency. IoU and confidence thresholds are fully runtime-configurable via environment variables.

- **HAL abstraction** — `IFrameSource` interface with compile-time backend switching (`FILE` / `OPENCV`). Decouples frame acquisition from inference logic; adding new backends (V4L2, shared memory) requires zero changes to the pipeline.

- **Runtime model auto-detection** — same binary handles both SSD MobileNet v2 (4 output tensors, post-NMS) and YOLOv8n (1 raw tensor, requires NMS) by inspecting output tensor shapes at load time.

- **Benchmark-driven optimization** — complete benchmark suite comparing Python baseline vs C++ L0-L4. Every design decision is backed by measured data: throughput (FPS), per-frame latency (mean/p50/p90/p99), stage breakdown, queue wait analysis, and resource usage.

## Quick Start

```bash
git clone <repo-url> && cd sIm_edge_ai_app
bash models/download_model.sh
pip install opencv-python numpy && python gen_test_frames.py
docker compose up --build
# Dashboard → http://localhost:8501
```

## Key Results

| Metric | Python Baseline | C++ L4 (Production) |
|---|---|---|
| Throughput (FPS) | 5.9 | 12.8 |
| Memory (steady) | 58 MiB | 50 MiB |
| Steady-state malloc | Yes (per-frame) | Zero |
| Jitter CV | 0.107 | 0.148 |

> Full benchmark analysis with L0-L4 comparison → [04_benchmark.md](04_benchmark.md)

## Documentation Index

| Document | Description |
|---|---|
| [01_overview.md](01_overview.md) | This file — 5-minute project overview |
| [02_architecture.md](02_architecture.md) | System architecture deep dive |
| [03_cpp_pipeline_deep_dive.md](03_cpp_pipeline_deep_dive.md) | C++ L0-L4 pipeline optimization (core highlight) |
| [04_benchmark.md](04_benchmark.md) | Complete benchmark results and analysis |
| [05_setup_and_commands.md](05_setup_and_commands.md) | Environment setup + command reference |
| [06_code_walkthrough.md](06_code_walkthrough.md) | File-by-file code walkthrough |

## Roadmap

### Completed
1. MVP: single-threaded FileFrameSource, single machine Docker
2. Multi-threaded pipeline (3 pthreads + RingBuffer with POSIX semaphores)
3. Webcam support (OpenCVFrameSource + compile-time HAL switching)
4. YOLOv8n TFLite support + runtime model auto-detect
5. NMS parameters fully runtime-configurable
6. Two-machine deployment (separate device + server hosts)
7. NiceGUI dashboard (FastAPI + WebSocket, ECharts, dark mode)
8. Pipeline Levels 0-4 (progressive optimization, compile-time switch, benchmark analysis)

### Next
- Prometheus + Grafana observability
- YOLOv8n INT8 quantization (inference bottleneck: ~78ms → target ~30-40ms)
- Kernel module (`ml_stats.ko`) + bpftrace for eBPF-based observability
- QEMU ARM64 cross-compile for embedded deployment simulation
