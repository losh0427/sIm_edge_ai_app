# Benchmark Results & Analysis

## Test Environment

| Parameter | Value |
|---|---|
| Platform | WSL2 on Windows (Intel CPU) |
| Model (Pipeline Levels) | YOLOv8n float32, 640x640, TFLite + XNNPACK |
| Model (Python vs C++) | SSD MobileNet v2 INT8, 300x300 |
| Server | gRPC on same network (<SERVER_IP>:50051) |
| Frame source (Pipeline Levels) | Webcam via OpenCVFrameSource |
| Frame source (Python vs C++) | JPEG files via FileFrameSource |
| Warmup | 10-50 frames (excluded from metrics) |

---

## Python Baseline vs C++ (SSD MobileNet v2, FILE mode)

Both agents process the same JPEG files through the same SSD MobileNet v2 INT8 model and send results to the same gRPC server.

| Metric | C++ (L4) | Python | Ratio |
|---|---|---|---|
| **Throughput (FPS)** | **9.3** | 5.9 | **1.58x** |
| E2E latency mean | 536 ms | 144 ms | 0.27x |
| E2E latency p99 | 676 ms | 231 ms | 0.34x |
| **Memory (mean)** | **50 MiB** | 58 MiB | **0.86x** |
| CPU % (mean) | 377% | 173% | 2.18x |
| Jitter CV | 0.087 | 0.107 | 0.81x |
| Tail ratio (p99/p50) | 1.27x | 1.65x | 0.77x |

### Key Observations

**C++ FPS is 1.58x higher** — the 3-thread pipeline overlaps capture, inference, and upload, while Python runs everything sequentially.

**C++ E2E latency is higher** — this is expected. The multi-threaded pipeline includes RingBuffer queue wait time in E2E measurement. Python's sequential pipeline has zero queue wait, so its E2E ≈ sum of stage times.

**C++ tail latency is more predictable** — p99/p50 ratio of 1.27x vs Python's 1.65x. No GC pauses, no GIL contention.

**C++ memory is lower** — 50 MiB vs 58 MiB. Pre-allocated buffers + zero-malloc steady state vs Python's per-frame object allocation.

### Resource Usage (Docker Stats)

| Metric | C++ | Python |
|---|---|---|
| CPU % | 359-390% | 168-177% |
| Memory | 49-50 MiB (0.64%) | 53-63 MiB (0.68-0.81%) |
| PIDs | 25 | 42-43 |

C++ uses more CPU (multi-threaded) but achieves higher throughput per core. Python uses fewer cores due to GIL constraints.

---

## C++ Pipeline Levels L0-L4 (YOLOv8n, Webcam)

Each level ran for ~60 seconds with webcam input. YOLOv8n float32 was used to stress the inference stage.

### Throughput

| Level | FPS | vs L0 |
|---|---|---|
| L0 | 10.9 | baseline |
| L1 | ~12.8 | +17% |
| L2 | ~12.8 | +17% |
| L3 | ~12.6 | +16% |
| L4 | ~12.8 | +17% |

All multi-threaded levels achieve similar throughput (~12.8 FPS) because **inference is the bottleneck** at ~78ms/frame. Multi-threading overlaps capture/preprocess/upload with inference, gaining +17%.

### Per-Frame Latency

| Metric | L0 | L1 | L2 | L3 | L4 |
|---|---|---|---|---|---|
| E2E mean | **92 ms** | 8,164 ms | 786 ms | 396 ms | **394 ms** |
| E2E p50 | **87 ms** | 7,551 ms | 790 ms | 403 ms | **400 ms** |
| E2E p90 | 105 ms | 15,193 ms | 840 ms | 446 ms | 441 ms |
| E2E p99 | 154 ms | 16,280 ms | 954 ms | 506 ms | 477 ms |
| Jitter CV | 0.166 | 0.564 | **0.081** | 0.160 | **0.148** |

> L0 has the lowest E2E latency because there is no queue wait. However, L0's throughput is 17% lower. In multi-threaded pipelines, E2E includes queue wait time — they are not directly comparable to L0.

### Stage Breakdown (mean, microseconds)

| Stage | L0 | L1 | L2 | L3 | L4 |
|---|---|---|---|---|---|
| Capture | 2,723 | 64,091 | 12,372 | 11,796 | 9,866 |
| Preprocess | 1,076 | 909 | 904 | 68,002* | 68,607* |
| **Inference** | **74,477** | **78,395** | **78,395** | **79,357** | **78,332** |
| NMS | 1 | 1 | 1 | 1 | 1 |
| Thumbnail | 2,193 | 3,384 | 3,765 | 4,214 | 4,283 |
| gRPC send | 11,431 | 12,568 | 13,053 | 12,340 | 11,914 |
| **Queue wait** | **3** | **8,004,498** | **677,378** | **220,072** | **221,273** |

> *L3/L4 preprocess includes `acquire_write_slot()` blocking time — the ~68ms reflects backpressure wait, not actual resize cost (~1ms).

### Queue Wait Analysis

```
L0:  3 us          ← no queue (sequential)
L1:  8,004,498 us  ← unbounded queue, frames pile up indefinitely
L2:  677,378 us    ← bounded (cap=8), producer blocked when full
L3:  220,072 us    ← RingBuffer (4 slots), stronger backpressure
L4:  221,273 us    ← same as L3 (same buffer structure)
```

---

## Key Findings

### 1. Inference is the Absolute Bottleneck

All levels show inference at **74-79ms**, accounting for **81% of L0's serial pipeline**. Optimizing any other stage has minimal impact on throughput:

| Stage | Time | % of L0 pipeline |
|---|---|---|
| Inference | 74-79 ms | **81%** |
| gRPC send | 11-13 ms | 13% |
| Capture | 2-12 ms | 3-13% |
| Thumbnail | 2-4 ms | 2-5% |
| Preprocess (actual) | ~1 ms | 1% |
| NMS | ~1 us | ~0% |

To meaningfully improve FPS, the next step is **INT8 quantization** of YOLOv8n (estimated 78ms → 30-40ms, potentially doubling FPS).

### 2. Unbounded Queue = Ticking Time Bomb

L1's `std::queue` with no capacity limit causes:
- E2E latency grows linearly: 2s → 5s → 7.5s → 11s → 15s...
- Memory usage grows without bound
- Eventually OOM-killed by the kernel

This is the most impactful demonstration in the project — a naive "just add threads" approach looks correct but is **dangerous in production**.

### 3. Fewer Buffer Slots = Lower Latency

| Configuration | Slots | Queue Wait | E2E p99 |
|---|---|---|---|
| L2 (bounded queue) | 8 | 677 ms | 954 ms |
| L3/L4 (RingBuffer) | 4 | 220 ms | 477-506 ms |

More slots allow more frames to queue up, increasing latency. Fewer slots create stronger backpressure, keeping latency low at the cost of burst tolerance. The optimal slot count depends on the producer/consumer speed ratio.

### 4. L3 vs L4 — Minimal Performance Difference

| Metric | L3 | L4 | Diff |
|---|---|---|---|
| E2E mean | 395,782 us | 394,276 us | **-0.4%** |
| E2E p50 | 402,807 us | 399,972 us | **-0.7%** |

The `resize()` → `memcpy` temporary allocation in L3 costs ~1ms — negligible compared to 78ms inference. The difference would be more significant on a faster inference engine (e.g., GPU/NPU where inference drops to <5ms).

### 5. Multi-Threading Throughput Gain is Bounded

The +17% throughput gain (10.9 → 12.8 FPS) comes from overlapping non-inference stages with inference. The theoretical maximum with 3-stage pipelining is:

```
FPS_max = 1 / max(stage_time) = 1 / 78ms = 12.8 FPS
```

Measured throughput matches theoretical maximum — the pipeline is optimally balanced for this inference speed.

---

## Summary Table

| Level | Throughput | E2E p50 | Queue Wait | Memory Safety | Production Ready |
|---|---|---|---|---|---|
| L0 | 10.9 FPS | 87 ms | 0 | heap/frame | Testing only |
| L1 | 12.8 FPS | 7,551 ms | 8,004 ms | OOM risk | **Never** |
| L2 | 12.8 FPS | 790 ms | 677 ms | heap/frame | With caveats |
| L3 | 12.6 FPS | 403 ms | 220 ms | temp alloc | Near-production |
| L4 | 12.8 FPS | 400 ms | 221 ms | **zero-malloc** | **Yes** |

**The pipeline level evolution demonstrates not throughput optimization (which is inference-bound), but rather:**
1. **Latency control** — bounded queues prevent unbounded latency growth
2. **Memory safety** — pre-allocated buffers eliminate steady-state malloc
3. **Backpressure** — RingBuffer auto-throttles when consumer can't keep up
4. **Production readiness** — zero-copy avoids heap fragmentation in long-running embedded systems
