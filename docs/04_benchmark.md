# Benchmark Results & Analysis

## Test Environment

| Parameter | Value |
|---|---|
| Platform | WSL2 on Windows (Intel CPU) |
| Model (Pipeline Levels) | YOLOv8n float32, 640x640, TFLite |
| Model (Python vs C++) | SSD MobileNet v2 INT8, 300x300 |
| Server | gRPC on same network |
| Frame source (Pipeline Levels) | Webcam via OpenCVFrameSource |
| Frame source (Python vs C++) | JPEG files via FileFrameSource |
| Warmup | 10-50 frames (excluded from metrics) |

---

## Python Baseline vs C++ (SSD MobileNet v2, FILE mode)

| Metric | C++ (L4) | Python | Ratio |
|---|---|---|---|
| **Throughput (FPS)** | **9.3** | 5.9 | **1.58x** |
| E2E latency mean | 536 ms | 144 ms | — |
| **Memory (mean)** | **50 MiB** | 58 MiB | **0.86x** |
| Tail ratio (p99/p50) | 1.27x | 1.65x | — |

- **C++ FPS 1.58x higher** — 3-thread pipeline overlaps capture, inference, and upload
- **C++ E2E latency higher** — expected, includes RingBuffer queue wait time
- **C++ tail latency more predictable** — no GC pauses, no GIL contention
- **C++ memory lower** — pre-allocated buffers + zero-malloc steady state

---

## C++ Pipeline Levels L0-L4 (YOLOv8n, Webcam)

### Throughput

All multi-threaded levels: ~12.8 FPS (+17% vs L0's 10.9). **Inference is the bottleneck** at ~78ms/frame.

### Per-Frame Latency

| Metric | L0 | L1 | L2 | L3 | L4 |
|---|---|---|---|---|---|
| E2E mean | **92 ms** | 8,164 ms | 786 ms | 396 ms | **394 ms** |
| E2E p50 | **87 ms** | 7,551 ms | 790 ms | 403 ms | **400 ms** |
| E2E p99 | 154 ms | 16,280 ms | 954 ms | 506 ms | 477 ms |
| Queue wait | 3 us | 8,004 ms | 677 ms | 220 ms | 221 ms |

> L0 has lowest E2E (no queue wait) but 17% lower throughput. Multi-threaded E2E includes queue wait — not directly comparable.

### Stage Breakdown (mean)

| Stage | L0 | L4 |
|---|---|---|
| Capture | 2.7 ms | 9.9 ms |
| Preprocess (actual) | 1.1 ms | ~1 ms* |
| **Inference** | **74.5 ms** | **78.3 ms** |
| Thumbnail | 2.2 ms | 4.3 ms |
| gRPC send | 11.4 ms | 11.9 ms |
| NMS | ~1 us | ~1 us |

> *L3/L4 preprocess measurement includes `acquire_write_slot()` blocking time (~68ms), reflecting backpressure — not actual resize cost.

---

## Key Findings

### 1. Inference is the Absolute Bottleneck
74-79ms across all levels, **81% of L0 pipeline**. Next step: INT8 quantization (estimated 78ms → 30-40ms).

### 2. Unbounded Queue = Ticking Time Bomb
L1 latency grows linearly (2s → 15s+), eventually OOM. Most impactful demonstration — naive "just add threads" is dangerous.

### 3. Fewer Slots = Lower Latency
L2 (8 slots): 677ms queue wait. L3/L4 (4 slots): 220ms. Stronger backpressure keeps latency low at cost of burst tolerance.

### 4. L3 vs L4 — < 1% Difference
The ~1ms temp alloc is negligible vs 78ms inference. L4's value is in eliminating heap fragmentation for long-running systems, not benchmarks.

### 5. Multi-Threading Gain is Bounded
Measured ~12.8 FPS matches theoretical max: `1 / 78ms = 12.8 FPS`. Pipeline is optimally balanced.

---

## Summary

| Level | Throughput | E2E p50 | Queue Wait | Memory Safety | Production Ready |
|---|---|---|---|---|---|
| L0 | 10.9 FPS | 87 ms | 0 | heap/frame | Testing only |
| L1 | 12.8 FPS | 7,551 ms | 8,004 ms | OOM risk | **Never** |
| L2 | 12.8 FPS | 790 ms | 677 ms | heap/frame | With caveats |
| L3 | 12.6 FPS | 403 ms | 220 ms | temp alloc | Near-production |
| L4 | 12.8 FPS | 400 ms | 221 ms | **zero-malloc** | **Yes** |

**The evolution demonstrates latency control, memory safety, backpressure, and production readiness — not throughput (which is inference-bound).**
