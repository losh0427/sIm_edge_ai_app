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

- **5 pipeline levels (L0-L4)** — progressive optimization from single-threaded loop to zero-malloc 3-thread pipeline with POSIX semaphore-based RingBuffer. Each level isolates a specific systems concept.

- **Hand-written NMS** — `compute_iou()` + `nms_filter()` in C++, no framework dependency. Thresholds runtime-configurable via env vars.

- **HAL abstraction** — `IFrameSource` interface with compile-time backend switching (`FILE` / `OPENCV`). Adding new backends requires zero pipeline changes.

- **Runtime model auto-detection** — same binary handles SSD MobileNet v2 and YOLOv8n by inspecting output tensor shapes at load time.

- **Benchmark-driven optimization** — Python baseline vs C++ L0-L4 with measured throughput, latency percentiles, stage breakdown, and resource usage.

## Key Results

| Metric | Python Baseline | C++ L4 (Production) |
|---|---|---|
| Throughput (FPS) | 5.9 | 12.8 |
| Memory (steady) | 58 MiB | 50 MiB |
| Steady-state malloc | Yes (per-frame) | Zero |

Full benchmark analysis → [04_benchmark.md](04_benchmark.md)

## Quick Start

```bash
git clone <repo-url> && cd sIm_edge_ai_app
bash models/download_model.sh
pip install opencv-python numpy && python gen_test_frames.py
docker compose up --build
# Dashboard → http://localhost:8501
```
