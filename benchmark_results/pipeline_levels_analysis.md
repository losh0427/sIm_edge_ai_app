# Pipeline Level 0-4 Benchmark Analysis

## Test Environment

- **Device**: WSL2 (webcam via OpenCVFrameSource)
- **Model**: YOLOv8n float32 (640x640, TFLite + XNNPACK)
- **Server**: <SERVER_IP>:50051 (gRPC, same network)
- **Warmup**: 10 frames
- **Each level ran for ~60 seconds before stopping with Ctrl+C**

---

## Design Differences Per Level

| Level | Threading | Queue / Buffer | Memory | Key Demonstration |
|---|---|---|---|---|
| 0 | Single thread | None | `std::vector` heap alloc per frame | Simplest baseline |
| 1 | 3x `std::thread` | `std::queue` + mutex + cond_var (unbounded) | `std::vector` heap alloc per frame | STL multi-threading, demonstrates unbounded queue problem |
| 2 | 3x `pthread_create` | bounded queue + `sem_t` (capacity=8) | `std::vector` heap alloc per frame | POSIX API + backpressure |
| 3 | 3x `pthread_create` | `RingBuffer<T,4>` + pre-allocated slots | `resize()` → `memcpy` to slot | Near zero-malloc |
| 4 | 3x `pthread_create` | `RingBuffer<T,4>` + pre-allocated slots | `resize_into()` direct write to slot | True zero-copy production |

---

## Results Overview

### Throughput

| Level | Throughput FPS | Calculation Method |
|---|---|---|
| L0 | **10.9** | 1 / E2E_mean (serial, E2E = full pipeline time) |
| L1 | **~12.8** | 1 / inference_mean (parallel, inference is bottleneck) |
| L2 | **~12.8** | Same as above |
| L3 | **~12.6** | Same as above |
| L4 | **~12.8** | Same as above |

**Conclusion**: All multi-threaded levels achieve nearly identical throughput (~12.8 FPS) because **the bottleneck is always inference (~78ms/frame)**. The benefit of multi-threading is overlapping capture, preprocess, thumbnail, and gRPC with inference, improving from 10.9 to 12.8 FPS (+17%).

### Per-Frame Latency

| Metric | L0 | L1 | L2 | L3 | L4 |
|---|---|---|---|---|---|
| E2E mean | **92 ms** | 8,164 ms | 786 ms | 396 ms | **394 ms** |
| E2E p50 | **87 ms** | 7,551 ms | 790 ms | 403 ms | **400 ms** |
| E2E p90 | 105 ms | 15,193 ms | 840 ms | 446 ms | 441 ms |
| E2E p99 | 154 ms | 16,280 ms | 954 ms | 506 ms | 477 ms |
| Jitter CV | 0.166 | 0.564 | **0.081** | 0.160 | **0.148** |

> **Note**: L0 has the lowest E2E because there is no queue wait. However, L0 is serial with lower throughput.
> Multi-threaded pipeline E2E includes queue wait time and cannot be directly compared to L0.

### Stage Breakdown (mean, us)

| Stage | L0 | L1 | L2 | L3 | L4 |
|---|---|---|---|---|---|
| Capture | 2,723 | 64,091 | 12,372 | 11,796 | 9,866 |
| Preprocess | 1,076 | 909 | 904 | 68,002 | 68,607 |
| Inference | 74,477 | 78,395 | 78,395 | 79,357 | 78,332 |
| NMS | 1 | 1 | 1 | 1 | 1 |
| Thumbnail | 2,193 | 3,384 | 3,765 | 4,214 | 4,283 |
| gRPC send | 11,431 | 12,568 | 13,053 | 12,340 | 11,914 |
| **Stage sum** | **91,901** | **159,348** | **108,489** | **175,710** | **173,004** |
| **Queue wait** | **3** | **8,004,498** | **677,378** | **220,072** | **221,273** |

> L3/L4 preprocess includes the blocking time from `acquire_write_slot()` (waiting for infer to consume a slot), hence showing ~68ms instead of the actual resize cost (~1ms). This accurately reflects the backpressure mechanism in action.

---

## Key Findings

### 1. L1 Unbounded Queue — Memory Grows Continuously

```
Frames   11- 245:  E2E mean =  2.2 sec
Frames  246- 480:  E2E mean =  5.0 sec
Frames  481- 715:  E2E mean =  7.5 sec
Frames  716- 950:  E2E mean = 11.0 sec
Frames  951-1187:  E2E mean = 15.0 sec  ← linear growth, never converges
```

The recv thread processes each frame in ~1ms (webcam 30fps), while infer takes ~78ms per frame. About ~12 frames pile up in the queue every second. The longer it runs, the longer subsequent frames wait. If not stopped, it will eventually OOM.

**Lesson**: An unbounded queue in a producer > consumer scenario is a ticking time bomb.

### 2. Bounded Queue (L2) vs RingBuffer (L3/L4) — Backpressure Strength

| | L2 (bounded, cap=8) | L3/L4 (RingBuffer, slots=4) |
|---|---|---|
| Queue wait mean | 677 ms | 220 ms |
| E2E p99 | 954 ms | 477-506 ms |

L2's capacity=8 allows more frames to queue up, resulting in longer queue wait. L3/L4 with only 4 slots provide stronger backpressure — recv blocks more frequently, but per-frame latency is lower.

**Lesson**: Fewer buffer slots = stronger backpressure = lower latency (at the cost of burst tolerance).

### 3. L3 vs L4 — Minimal Difference

| | L3 | L4 | Difference |
|---|---|---|---|
| E2E mean | 395,782 us | 394,276 us | -0.4% |
| E2E p50 | 402,807 us | 399,972 us | -0.7% |
| Inference | 79,357 us | 78,332 us | -1.3% |

L3 (`resize()` → `memcpy`) and L4 (`resize_into()` direct write) show virtually no difference in throughput and latency. Because inference at 78ms completely dominates the pipeline, the 1ms heap alloc from resize is negligible.

**However, L4's significance lies in**:
- Steady-state **zero malloc** (recv/infer threads), avoiding heap fragmentation in embedded or long-running scenarios
- Demonstrating that "pre-allocated buffers are a necessary condition, but the API must also support in-place writes to achieve true zero-malloc"

### 4. Inference is the Absolute Bottleneck

All levels show inference at **74-79ms**, accounting for **81%** of the L0 serial pipeline.

Optimizing stages other than inference (preprocess 1ms, thumbnail 2-4ms, gRPC 11-13ms) has very limited impact on overall throughput. To truly improve FPS, the following are needed:
- Faster models (INT8 quantization, smaller architectures)
- GPU/NPU acceleration
- Model batch inference

---

## Summary

```
            Throughput    Latency     Memory Safety     Complexity
            (FPS)        (E2E p50)
  L0        10.9         87 ms       heap/frame        Simplest
  L1        12.8         7,551 ms    heap/frame        OOM risk
  L2        12.8         790 ms      heap/frame        Medium
  L3        12.6         403 ms      temp alloc        Medium-high
  L4        12.8         400 ms      zero-malloc       Highest

  Bottleneck: inference ~78ms/frame (YOLOv8n float32 on CPU)
  Multi-threading throughput gain: +17% (10.9 → 12.8 FPS)
```

**The pipeline level evolution is not about improving throughput (which is determined by inference), but rather about**:
1. **Latency control**: L2+ bounded queues prevent unbounded latency growth
2. **Memory safety**: L3/L4 pre-allocated buffers eliminate steady-state malloc
3. **Backpressure**: L3/L4 RingBuffer auto-throttles when producer > consumer
4. **Production readiness**: L4 zero-copy avoids heap fragmentation in embedded/long-running environments
