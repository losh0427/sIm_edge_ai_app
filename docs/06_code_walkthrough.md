# Code Walkthrough

A file-by-file guide to the codebase. Each entry includes the file's purpose and key functions.

## Directory Structure

```
├── proto/edge_ai.proto
├── docker-compose.yml
│
├── device/
│   ├── Dockerfile
│   ├── CMakeLists.txt
│   ├── hal/
│   │   ├── i_frame_source.h
│   │   ├── file_frame_source.h / .cpp
│   │   └── opencv_frame_source.h / .cpp
│   └── src/
│       ├── main.cpp
│       ├── pipeline.h
│       ├── pipeline.cpp          (L4 — production)
│       ├── pipeline_l0.cpp       (L0 — single-threaded)
│       ├── pipeline_l1.cpp       (L1 — std::thread + unbounded queue)
│       ├── pipeline_l2.cpp       (L2 — pthread + bounded queue)
│       ├── pipeline_l3.cpp       (L3 — RingBuffer + memcpy)
│       ├── ring_buffer.h
│       ├── pipeline_slots.h
│       ├── preprocess.h / .cpp
│       ├── inference.h / .cpp
│       ├── postprocess.h / .cpp
│       ├── grpc_client.h / .cpp
│       └── labels.h
│
├── server/
│   ├── Dockerfile
│   ├── entrypoint.py
│   └── app/
│       ├── main.py
│       ├── grpc_server.py
│       ├── state.py
│       └── draw.py
│
├── python_device/
│   ├── Dockerfile
│   ├── main.py
│   └── requirements.txt
│
├── models/
│   ├── download_model.sh
│   ├── export_yolov8n.py
│   ├── coco_labels.txt
│   └── coco80_labels.txt
│
├── benchmark_results/
│   ├── pipeline_levels_analysis.md
│   ├── cpp_<timestamp>/
│   └── python_<timestamp>/
│
├── bench_common.sh
├── bench_cpp.sh
├── bench_python.sh
├── test_grpc_client.py
├── gen_test_frames.py
└── requirements-dev.txt
```

---

## proto/

### `edge_ai.proto`
gRPC service contract between device and server.

- **`EdgeService`** — two RPCs: `ReportDetection` (per-frame results) and `ReportStats` (periodic health)
- **`DetectionFrame`** — edge_id, bounding boxes (normalized coords), JPEG thumbnail, inference latency, frame number, HAL backend info
- **`BoundingBox`** — x_min/y_min/x_max/y_max (normalized [0,1]), class_id, label, confidence
- **`EdgeStats`** — avg/p99 latency, FPS, class distribution map
- **`ServerResponse`** — simple ok/message ack

---

## device/

### `Dockerfile`
Multi-stage build:
1. **Builder stage** (ubuntu:22.04) — installs build tools, compiles TFLite v2.14.0 from source (~15 min, Docker layer cached), builds `edge_agent` binary
2. **Runtime stage** (ubuntu:22.04) — copies binary + runtime libs only, minimal image

Build args: `HAL_CAMERA_BACKEND` and `PIPELINE_LEVEL` passed to CMake.

### `CMakeLists.txt`
Build system configuration.

- `HAL_CAMERA_BACKEND` → defines `HAL_USE_FILE` or `HAL_USE_OPENCV`, selects source files
- `PIPELINE_LEVEL` → selects `pipeline_l{N}.cpp` or `pipeline.cpp` (default L4)
- Links: TFLite (static, from `/tf`), OpenCV, gRPC, Protobuf
- `add_custom_command` generates C++ proto stubs from `edge_ai.proto`

### `hal/i_frame_source.h`
Abstract interface for frame acquisition.

- **`Frame`** struct — `std::vector<uint8_t> data`, width, height, channels (always 3, RGB)
- **`IFrameSource`** interface — `open()`, `next_frame(Frame&)`, `close()`, `describe()`

### `hal/file_frame_source.h / .cpp`
Reads JPEG/PNG files from a directory in sorted order, loops when exhausted.

- `open()` — scans directory for image files, sorts alphabetically
- `next_frame()` — `cv::imread` → `cv::resize` → `cv::cvtColor(BGR2RGB)` → fills `Frame`

### `hal/opencv_frame_source.h / .cpp`
Captures from V4L2 webcam via OpenCV VideoCapture.

- `open()` — `cv::VideoCapture::open(cam_index, CAP_V4L2)`, sets MJPEG format and buffer size 1
- `next_frame()` — `cap_.read()` → resize → `cvtColor(BGR2RGB)` → fills `Frame`
- MJPEG format chosen to avoid YUYV bandwidth issues over USB/usbipd

### `src/main.cpp`
Entry point. Parses environment variables, initializes HAL + Inference + gRPC + Pipeline.

- **Key functions**: `main()`, `sig_handler()`
- Signal handler (`SIGINT`/`SIGTERM`) calls `pipeline.request_shutdown()` for clean exit
- Compile-time `#if defined(HAL_USE_OPENCV)` selects frame source type

### `src/pipeline.h`
Stable interface for all pipeline levels — `start()`, `stop()`, `request_shutdown()`.

- **`PipelineConfig`** — holds pointers to source/inference/grpc, thresholds, labels, benchmark settings
- **`Pipeline`** — Pimpl pattern (`struct Impl` forward declaration), implementation varies by level

### `src/pipeline.cpp` (Level 4)
Production pipeline: 3x `pthread_create` + 2x `RingBuffer` + zero-malloc.

- **`recv_loop()`** — acquire write slot → `resize_into()` direct write → commit
- **`infer_loop()`** — acquire read slot → `Inference::run()` → NMS → fill UploadSlot → commit
- **`upload_loop()`** — acquire read slot → `encode_thumbnail_raw()` → `gRPC send` → commit
- **`start()`** — init RingBuffers, create 3 pthreads
- **`stop()`** — `pthread_join` all 3 threads, destroy RingBuffers

### `src/pipeline_l0.cpp`
Single-threaded blocking loop. Simplest implementation.

- One `while` loop: capture → resize (heap) → infer → NMS → thumbnail → gRPC
- Per-frame CSV logging when `BENCH_CSV` is set

### `src/pipeline_l1.cpp`
3x `std::thread` + `ThreadSafeQueue<T>` (unbounded).

- **`ThreadSafeQueue`** — `std::queue` + `std::mutex` + `std::condition_variable`
- Demonstrates unbounded queue problem: E2E latency grows linearly over time

### `src/pipeline_l2.cpp`
3x `pthread_create` + `PosixQueue<T>` (bounded, capacity=8).

- **`PosixQueue`** — `std::queue` + `pthread_mutex_t` + `sem_t` (empty_count + fill_count)
- `sem_wait(&empty_count_)` blocks producer when queue is full (backpressure)

### `src/pipeline_l3.cpp`
3x `pthread_create` + `RingBuffer<T,4>` + pre-allocated slots.

- Uses `resize()` (returns vector) + `memcpy()` into pre-allocated slot
- Nearly zero-malloc but still has temporary allocation per frame

### `src/ring_buffer.h`
Template SPSC RingBuffer with POSIX semaphores.

- **`acquire_write_slot()`** — `sem_wait(&empty_count_)` then return slot pointer
- **`commit_write_slot()`** — `sem_post(&fill_count_)`
- **`acquire_read_slot()`** — `sem_wait(&fill_count_)` then return slot pointer
- **`commit_read_slot()`** — `sem_post(&empty_count_)`
- **`shutdown()`** — sets flag + posts to both semaphores to unblock threads

### `src/pipeline_slots.h`
Pre-allocated slot structures with compile-time sized buffers.

- **`InferSlot`** — `input_data[640*640*3]` (1.2 MB), `orig_data[640*480*3]`, frame metadata
- **`UploadSlot`** — `Detection[100]`, `orig_data[640*480*3]`, timing metadata
- `kMaxModelInputSize = 640 * 640 * 3` — sized for largest supported model

### `src/preprocess.h / .cpp`
Image resizing and JPEG encoding utilities.

- **`resize()`** — returns `std::vector<uint8_t>` (heap alloc), used by L0-L3
- **`resize_into()`** — writes directly into pre-allocated buffer (zero alloc), used by L4
- **`encode_thumbnail()`** — takes Frame, resizes + JPEG encodes
- **`encode_thumbnail_raw()`** — takes raw RGB data pointer, resizes + JPEG encodes (used by L3/L4)

### `src/inference.h / .cpp`
TFLite inference engine with runtime model auto-detection.

- **`load_model()`** — loads `.tflite`, creates interpreter, detects model type by output tensor count
- **`run()`** — copies input data to tensor (with float normalization if needed), calls `Invoke()`, parses output
- **`parse_ssd_output()`** — extracts from 4 output tensors (boxes, classes, scores, count)
- **`parse_yolov8_output()`** — transposes `[1, 84, 8400]`, finds per-anchor argmax, filters by confidence

### `src/postprocess.h / .cpp`
Hand-written Non-Maximum Suppression.

- **`compute_iou()`** — intersection-over-union between two Detection boxes
- **`nms_filter()`** — score filter → sort by confidence → greedy suppression by IoU threshold

### `src/grpc_client.h / .cpp`
gRPC client for sending DetectionFrame to server.

- **`connect()`** — creates gRPC channel + stub
- **`send_detection()`** — builds DetectionFrame protobuf, sets 5-second deadline, calls `ReportDetection`

### `src/labels.h`
Simple label file loader — reads lines from text file into `std::vector<std::string>`.

---

## server/

### `Dockerfile`
Python server image. Installs dependencies and generates proto Python stubs at build time using `grpc_tools.protoc`. Stubs are not committed to git.

### `entrypoint.py`
Startup orchestration.

- Creates shared `Store` instance
- Starts gRPC server on `:50051` in a background `threading.Thread`
- Imports `app.main` (registers NiceGUI routes)
- Calls `ui.run()` on `:8501`

### `app/main.py`
NiceGUI dashboard (294 lines).

- **`@ui.page('/')`** — renders per-device cards with status, metrics, detection list
- **`ui.timer(0.5)`** — polls Store, updates DOM elements
- **ECharts** — FPS and latency time-series charts
- **`@app.get('/annotated/{edge_id}')`** — serves JPEG with drawn bounding boxes
- Dark mode enabled by default

### `app/grpc_server.py`
gRPC service implementation (42 lines).

- **`EdgeServicer.ReportDetection()`** — deserializes DetectionFrame → creates DetectionResult → `store.update_detection()`
- **`EdgeServicer.ReportStats()`** — placeholder for device health reports

### `app/state.py`
Thread-safe shared state (52 lines).

- **`DetectionResult`** — dataclass with edge_id, boxes, thumbnail bytes, latency, frame_number
- **`Store`** — mutex-guarded dict keyed by edge_id, `threading.Event` for frame arrival notification
- Methods: `update_detection()`, `get_latest()`, `wait_new_frame()`

### `app/draw.py`
Pure drawing utilities (64 lines).

- **`draw_boxes()`** — OpenCV `cv2.rectangle` + `cv2.putText` for bounding box overlay
- **`render_annotated_jpeg()`** — decode JPEG → draw boxes → re-encode JPEG
- **`load_labels()` / `get_label()`** — COCO label file management

---

## python_device/

### `main.py`
Python baseline device agent (346 lines). Equivalent to C++ device for benchmark comparison.

- Single-threaded TFLite pipeline: capture → preprocess → infer → NMS → thumbnail → gRPC
- Supports FILE and OPENCV backends (runtime switch)
- Runtime model auto-detection (SSD vs YOLOv8)
- Hand-written NMS matching C++ implementation
- CSV benchmark logging with per-stage microsecond timing
- Memory tracking via `resource.getrusage()`

### `Dockerfile`
Python device container: `python:3.11-slim` + `tflite-runtime` + `grpcio` + `opencv-python-headless`.

### `requirements.txt`
Dependencies: tflite-runtime 2.14.0, grpcio 1.60.0, protobuf 4.25.0, opencv-python-headless 4.9.0, numpy 1.26.0.

---

## Scripts

### `bench_common.sh`
Shared benchmark functions (201 lines).

- `setup()` — creates output directory, writes config
- `ensure_server_up()` — starts server container if needed
- `start_device()` — launches device with `BENCH_CSV` env var
- `wait_for_warmup()` — polls CSV for warmup completion
- `stop_and_collect()` — stops device, collects logs, runs analysis
- `cleanup()` — Ctrl+C handler

### `bench_cpp.sh` / `bench_python.sh`
Thin wrappers around `bench_common.sh` — set agent type (cpp/python) and device service name, then call shared functions.

### `test_grpc_client.py`
Python fake gRPC client (51 lines). Generates synthetic JPEG thumbnails with "TEST" text, sends 100 DetectionFrame messages. Used to test server independently without building the C++ device.

### `gen_test_frames.py`
Test frame generator (128 lines). Downloads 20 COCO val2017 sample images (or captures from webcam with `--webcam` flag), resizes to 640x480, saves to `data/test_frames/`.

---

## Models

### `download_model.sh`
Downloads SSD MobileNet v2 INT8 TFLite model from TensorFlow Hub.

### `export_yolov8n.py`
Exports YOLOv8n to float32 TFLite format using the Ultralytics library. Output: `models/yolov8n_float32.tflite`.

### `coco_labels.txt`
91-class COCO labels for SSD MobileNet v2 (includes background class at index 0).

### `coco80_labels.txt`
80-class COCO labels for YOLOv8 (no background class).

---

## Configuration Files

### `docker-compose.yml`
Orchestrates 3 services on `edge-net` bridge network:
- **server** — ports 8501 (dashboard) + 50051 (gRPC)
- **device** — C++ agent with build args for HAL and pipeline level
- **py-device** — Python baseline agent (for benchmarking)

### `requirements-dev.txt`
Local dev dependencies: numpy, opencv-python, grpcio/grpcio-tools, protobuf, ultralytics, tensorflow (for model export).
