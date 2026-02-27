# System Architecture

## Overview

The system consists of two Docker containers running on the same `edge-net` bridge network (or across two physical machines):

```
┌─────────────────────┐         gRPC          ┌─────────────────────┐
│   Device (C++17)    │ ──────────────────── ▶ │   Server (Python)   │
│   edge_agent binary │    TCP :50051          │   NiceGUI :8501     │
└─────────────────────┘                        └─────────────────────┘
```

- **Device** — captures frames, runs TFLite inference, sends detection results
- **Server** — receives gRPC messages, stores state, renders live dashboard

---

## Device Architecture

### HAL Layer (Hardware Abstraction)

```
           IFrameSource (interface)
           ├── FileFrameSource      — reads JPEGs from directory (testing/benchmark)
           └── OpenCVFrameSource    — V4L2 webcam via OpenCV VideoCapture (production)
```

The HAL backend is selected at **compile time** via CMake:

```cmake
cmake -DHAL_CAMERA_BACKEND=FILE ..    # FileFrameSource
cmake -DHAL_CAMERA_BACKEND=OPENCV ..  # OpenCVFrameSource
```

This generates `#define HAL_USE_FILE` or `#define HAL_USE_OPENCV`, and `main.cpp` uses `#if defined(...)` to instantiate the correct source. Zero runtime overhead — the unused backend is never compiled.

**IFrameSource interface**:
```cpp
class IFrameSource {
public:
    virtual bool open() = 0;
    virtual bool next_frame(Frame& frame) = 0;
    virtual void close() = 0;
    virtual std::string describe() const = 0;
};
```

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

Three `pthread` threads run in parallel:
1. **recv** — acquires frame from HAL, calls `resize_into()` to write directly into pre-allocated `InferSlot` buffer (zero heap allocation)
2. **infer** — runs TFLite `Invoke()` on slot data, applies hand-written NMS, copies results to `UploadSlot`
3. **upload** — encodes JPEG thumbnail, sends `DetectionFrame` via gRPC

The RingBuffer provides natural **backpressure**: if `infer` is slow, `recv` blocks on `acquire_write_slot()` (semaphore wait) — preventing unbounded queue growth.

### Inference Engine

The `Inference` class auto-detects model type at load time:

| Model | Input | Output | NMS |
|---|---|---|---|
| SSD MobileNet v2 | 300x300 uint8 | 4 tensors (boxes, classes, scores, count) — post-NMS | Built into model |
| YOLOv8n | 640x640 float32 | 1 tensor `[1, 84, 8400]` — raw anchors | Hand-written C++ NMS applied |

Detection logic:
```
output tensor count == 4  →  SSD_MOBILENET (parse 4 separate tensors)
output tensor count == 1  →  YOLOV8 (transpose + per-class argmax + NMS)
```

For float32 models, input data is normalized from `[0,255]` to `[0.0, 1.0]` before inference.

### Hand-Written NMS

```cpp
namespace postprocess {
    float compute_iou(const Detection& a, const Detection& b);
    std::vector<Detection> nms_filter(std::vector<Detection>& dets,
                                       float iou_threshold, float score_threshold);
}
```

Algorithm:
1. Filter detections below `score_threshold` (CONF_THRESH env var)
2. Sort by confidence descending
3. Greedy suppression: for each kept detection, suppress all remaining detections with IoU > `iou_threshold`

No framework dependency — the NMS is ~40 lines of C++ with `std::sort` and `std::max/min`.

### Pre-Allocated Slots

```cpp
// pipeline_slots.h
static constexpr int kMaxModelInputSize = 640 * 640 * 3;  // 1.2 MB

struct InferSlot {
    uint8_t input_data[kMaxModelInputSize];  // model input (pre-allocated)
    uint8_t orig_data[kOrigDataSize];        // original frame for thumbnail
    int     frame_number;
    int64_t ts_capture_start, ts_capture_end, ts_preprocess_end;
};

struct UploadSlot {
    Detection detections[kMaxDetections];    // detection results (pre-allocated)
    int       detection_count;
    uint8_t   orig_data[kOrigDataSize];      // for thumbnail encoding
    // ... timing metadata
};
```

Buffer sizes are set to accommodate the largest supported model (YOLOv8n 640x640). Smaller models (SSD 300x300) simply use a subset of the buffer.

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

### Components

**`grpc_server.py` — EdgeServicer**
- Implements `ReportDetection` and `ReportStats` RPCs
- Deserializes `DetectionFrame` protobuf → creates `DetectionResult` dataclass
- Calls `store.update_detection()` to write into shared state

**`state.py` — Thread-Safe Store**
```python
class Store:
    _lock: threading.Lock
    _latest: dict[str, DetectionResult]  # keyed by edge_id
    _event: threading.Event              # signals new frame arrival

    def update_detection(self, result: DetectionResult) -> None
    def get_latest(self, edge_id: str) -> DetectionResult | None
    def wait_new_frame(self, timeout: float) -> bool
```

All access is mutex-guarded. The `threading.Event` allows the dashboard to efficiently wait for new data rather than busy-polling.

**`main.py` — NiceGUI Dashboard**
- `@ui.page('/')` renders per-device cards with status indicators
- `ui.timer(0.5)` callback reads from Store, updates DOM
- ECharts components for FPS and latency visualization
- FastAPI endpoints: `/annotated/{edge_id}` serves JPEG with drawn bounding boxes

**`draw.py` — Drawing Utilities**
- `draw_boxes()` — OpenCV rectangle + putText for bounding box overlay
- `render_annotated_jpeg()` — decodes thumbnail, draws boxes, re-encodes JPEG

---

## Protocol Buffer Definition

```protobuf
// proto/edge_ai.proto
service EdgeService {
    rpc ReportDetection (DetectionFrame) returns (ServerResponse);
    rpc ReportStats     (EdgeStats)      returns (ServerResponse);
}

message BoundingBox {
    float x_min = 1;  // normalized [0,1]
    float y_min = 2;
    float x_max = 3;
    float y_max = 4;
    int32 class_id = 5;
    string label = 6;
    float confidence = 7;
}

message DetectionFrame {
    string edge_id = 1;
    int64 timestamp_ms = 2;
    repeated BoundingBox boxes = 3;
    bytes thumbnail_jpeg = 4;
    float inference_latency_ms = 5;
    int32 frame_number = 6;
    string hal_backend = 7;
    int32 original_width = 8;
    int32 original_height = 9;
}
```

---

## Compile-Time Configuration

| CMake Flag | Values | Effect |
|---|---|---|
| `HAL_CAMERA_BACKEND` | `FILE`, `OPENCV` | Selects IFrameSource implementation |
| `PIPELINE_LEVEL` | `0`, `1`, `2`, `3`, `4` | Selects pipeline implementation file |

Both are set via Docker Compose `build.args` in `docker-compose.yml`:
```yaml
device:
  build:
    args:
      HAL_CAMERA_BACKEND: ${HAL_CAMERA_BACKEND:-FILE}
      PIPELINE_LEVEL: ${PIPELINE_LEVEL:-4}
```

---

## Docker Build Process

### Device (Multi-Stage)

```
Stage 1: Builder (ubuntu:22.04)
├── Install build-essential, cmake, libopencv-dev, libgrpc++-dev
├── Copy TFLite v2.14.0 source → build libtensorflowlite.a (~15 min, cached)
├── Copy project source
├── cmake -DHAL_CAMERA_BACKEND=... -DPIPELINE_LEVEL=... && make
└── Output: edge_agent binary

Stage 2: Runtime (ubuntu:22.04)
├── Install runtime libs only (libopencv, libgrpc++)
├── Copy edge_agent from builder
└── ENTRYPOINT ["./edge_agent"]
```

### Server

```
python:3.11-slim
├── pip install nicegui grpcio opencv-python-headless
├── grpc_tools.protoc → generate edge_ai_pb2.py + edge_ai_pb2_grpc.py
└── ENTRYPOINT ["python", "entrypoint.py"]
```

Proto Python stubs are generated at **Docker build time**, not committed to git.

---

## Network Architecture

### Single Machine
```
docker network: edge-net (bridge)
├── server:50051  (gRPC)
├── server:8501   (NiceGUI dashboard)
└── device → server:50051
```

### Two Machines
```
Machine A (Server):  192.168.x.x
├── Docker → server:50051, server:8501
└── Windows port proxy (if WSL2): 0.0.0.0:50051 → WSL_IP:50051

Machine B (Device):  192.168.y.y
├── Docker → device (with --no-deps)
├── SERVER_ADDR=192.168.x.x:50051
└── Webcam: /dev/video0 → HAL_CAMERA_BACKEND=OPENCV
```
