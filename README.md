# Real-Time Multi-Device Object Detection Application 

Real-time object detection on simulated IoT edge devices.
A C++ edge agent captures frames, runs TFLite inference, and streams results over gRPC to a Python server with a live NiceGUI dashboard.

## Demo

https://github.com/user-attachments/assets/fc5039b3-14d7-4d51-b3e0-54522aab2edf

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
| Models | SSD MobileNet v2 (INT8) / YOLOv8n (float32) |

## Quick Start

```bash
# 1. Clone & prepare
git clone <repo-url> && cd sIm_edge_ai_app
bash models/download_model.sh
pip install opencv-python numpy && python gen_test_frames.py

# 2. Build & run
docker compose up --build

# 3. Open dashboard
#    http://localhost:8501
```

For YOLOv8n, two-machine deployment, webcam setup, and pipeline level switching, see [Setup & Commands](docs/05_setup_and_commands.md).

## Key Results

| Metric | Python Baseline | C++ L4 (Production) |
|---|---|---|
| Throughput (FPS) | 5.9 | 12.8 |
| Memory (steady) | 58 MiB | 50 MiB |
| Steady-state malloc | Yes (per-frame) | Zero |

5 pipeline levels (L0-L4) demonstrate progressive optimization from single-threaded to zero-malloc production pipeline. Full analysis in [Benchmark](docs/04_benchmark.md).

## Documentation

| Document | Description |
|---|---|
| [Project Overview](docs/01_overview.md) | 5-minute summary for interviews |
| [System Architecture](docs/02_architecture.md) | Architecture deep dive (HAL, pipeline, server, proto, Docker) |
| [C++ Pipeline Deep Dive](docs/03_cpp_pipeline_deep_dive.md) | L0-L4 optimization evolution (core technical highlight) |
| [Benchmark Results](docs/04_benchmark.md) | Python vs C++, L0-L4 comparison, stage breakdown analysis |
| [Setup & Commands](docs/05_setup_and_commands.md) | Environment setup, build commands, troubleshooting |

## Environment Variables

| Variable | Default | Description |
|---|---|---|
| `SERVER_ADDR` | `server:50051` | gRPC server address |
| `EDGE_ID` | `edge-1` | Device identifier |
| `MODEL_PATH` | `/app/models/ssd_mobilenet_v2.tflite` | TFLite model path |
| `LABELS_PATH` | `/app/models/coco_labels.txt` | Labels file |
| `CONF_THRESH` | `0.4` | Detection confidence threshold |
| `IOU_THRESH` | `0.45` | NMS IoU threshold |
| `HAL_CAMERA_BACKEND` | `FILE` | Compile-time: `FILE` or `OPENCV` |
| `PIPELINE_LEVEL` | `4` | Compile-time: 0 (single-thread) to 4 (production) |

Full variable reference in [Setup & Commands](docs/05_setup_and_commands.md).

## License

MIT
