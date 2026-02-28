# C++ Pipeline Deep Dive (L0-L4)

Each level isolates a specific systems programming concept. This is the core technical highlight of the project.

## Evolution Overview

| Level | Threading | Queue / Buffer | Memory Model | Key Concept |
|---|---|---|---|---|
| L0 | Single thread | None | heap alloc per frame | Baseline |
| L1 | 3x `std::thread` | `std::queue` + mutex + cond_var (unbounded) | heap alloc per frame | Unbounded queue danger |
| L2 | 3x `pthread_create` | Bounded queue + `sem_t` (capacity=8) | heap alloc per frame | POSIX API + backpressure |
| L3 | 3x `pthread_create` | `RingBuffer<T,4>` + pre-allocated slots | `resize()` → `memcpy` to slot | Near zero-malloc |
| L4 | 3x `pthread_create` | `RingBuffer<T,4>` + pre-allocated slots | `resize_into()` direct write | True zero-malloc |

## Performance Summary

```
            Throughput    Latency     Memory Safety     Complexity
            (FPS)        (E2E p50)
  L0        10.9         87 ms       heap/frame        Simplest
  L1        12.8         7,551 ms    heap/frame        OOM risk
  L2        12.8         790 ms      heap/frame        Medium
  L3        12.6         403 ms      temp alloc        Medium-high
  L4        12.8         400 ms      zero-malloc       Highest
```

---

## Level 0: Single-Threaded Sequential

**File**: `pipeline_l0.cpp`

A blocking loop: `capture → resize → infer → NMS → thumbnail → gRPC → repeat`. Throughput capped by sum of all stages. While inference runs (~78ms), everything else sits idle.

**Takeaway**: Baseline with lowest per-frame latency (no queue wait), but 17% lower throughput than multi-threaded levels.

---

## Level 1: std::thread + Unbounded Queue

**File**: `pipeline_l1.cpp`

Three `std::thread` connected by unbounded `ThreadSafeQueue`. Recv captures at ~30 FPS (webcam), infer processes at ~12.8 FPS — ~17 frames pile up per second.

```
E2E mean over time:  2.2s → 5.0s → 7.5s → 11.0s → 15.0s  ← linear growth, never converges
```

**Takeaway**: Unbounded queue between fast producer and slow consumer is a **ticking time bomb**. Throughput looks fine, but latency degrades to seconds and will eventually OOM.

---

## Level 2: POSIX pthread + Bounded Queue

**File**: `pipeline_l2.cpp`

Replaces STL with POSIX APIs. Adds bounded capacity (8 slots) via `sem_t` — producer blocks when full. **93% reduction in queue wait** vs L1.

| | L1 (unbounded) | L2 (bounded, cap=8) |
|---|---|---|
| Queue wait | 8,004 ms | 677 ms |
| E2E p50 | 7,551 ms | 790 ms |

**Why POSIX over STL?** Access to thread attributes, priority inheritance, `sem_timedwait()`, and demonstrates familiarity with embedded Linux primitives (ChromeOS, Nest Hub).

---

## Level 3: RingBuffer + Pre-Allocated Slots (memcpy)

**File**: `pipeline_l3.cpp`

Replaces `std::queue` with fixed-size `RingBuffer` (see `ring_buffer.h`) and pre-allocated `InferSlot`/`UploadSlot` structs (see `pipeline_slots.h`). 4 slots allocated once at startup (~8 MB total).

Still calls `resize()` → `memcpy` (temporary heap alloc per frame). The improvement vs L2 comes from **fewer slots** (4 vs 8) providing stronger backpressure:

| | L2 (8 slots) | L3 (4 slots) |
|---|---|---|
| Queue wait | 677 ms | 220 ms |
| E2E p50 | 790 ms | 403 ms |

---

## Level 4: Zero-Copy Production

**File**: `pipeline.cpp`

`resize_into()` writes **directly** into the pre-allocated slot buffer — wraps destination with `cv::Mat` header (no allocation). True zero-malloc in recv/infer threads.

### L3 vs L4

| Metric | L3 | L4 | Diff |
|---|---|---|---|
| E2E mean | 395,782 us | 394,276 us | -0.4% |
| E2E p50 | 402,807 us | 399,972 us | -0.7% |

**< 1% difference** because inference (78ms) dominates. The `resize()` temp alloc costs ~1ms — negligible.

### Why L4 Matters Despite Minimal Performance Gain

1. **Steady-state zero-malloc** — prevents heap fragmentation in long-running embedded systems
2. **API design principle** — pre-allocated buffers alone aren't enough; the API must support in-place writes
3. **Upload thread exception** — still allocates for JPEG encode + protobuf (I/O-bound, acceptable)

---

## Pipeline Level Selection

Compile-time switch via `PIPELINE_LEVEL` build arg. CMake maps level to source file. All levels implement the same `Pipeline` class interface (see `pipeline.h`) — only the `.cpp` changes.

```bash
PIPELINE_LEVEL=0 docker compose build device   # Level 0
PIPELINE_LEVEL=2 docker compose build device   # Level 2
docker compose build device                     # Level 4 (default)
```
