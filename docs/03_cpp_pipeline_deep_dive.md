# C++ Pipeline Deep Dive (L0-L4)

This document walks through the 5 pipeline levels, explaining the **why** behind each optimization. This is the core technical highlight of the project — each level isolates a specific systems programming concept.

## Evolution Overview

| Level | Threading | Queue / Buffer | Memory Model | Key Concept |
|---|---|---|---|---|
| L0 | Single thread | None | `std::vector` heap alloc per frame | Baseline |
| L1 | 3x `std::thread` | `std::queue` + mutex + cond_var (unbounded) | `std::vector` heap alloc per frame | STL threading, unbounded queue danger |
| L2 | 3x `pthread_create` | Bounded queue + `sem_t` (capacity=8) | `std::vector` heap alloc per frame | POSIX API + backpressure |
| L3 | 3x `pthread_create` | `RingBuffer<T,4>` + pre-allocated slots | `resize()` → `memcpy` to slot | Near zero-malloc |
| L4 | 3x `pthread_create` | `RingBuffer<T,4>` + pre-allocated slots | `resize_into()` direct write | True zero-malloc production |

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

**File**: `device/src/pipeline_l0.cpp`

### Design

The simplest possible implementation — a blocking loop that processes one frame at a time:

```
capture → resize → infer → NMS → thumbnail → gRPC → (repeat)
```

```cpp
// pipeline_l0.cpp — core loop (simplified)
while (!impl_->shutdown_requested) {
    Frame frame;
    cfg.source->next_frame(frame);

    auto resized = preprocess::resize(frame, infer_w, infer_h);  // heap alloc
    auto dets = cfg.inference->run(resized.data(), resized.size(), cfg.conf_threshold);
    dets = postprocess::nms_filter(dets, cfg.iou_threshold, cfg.conf_threshold);

    auto thumb = preprocess::encode_thumbnail(frame);             // heap alloc
    cfg.grpc->send_detection(cfg.edge_id, dets, thumb, latency, frame_num, ...);
}
```

### Analysis

| Metric | Value | Note |
|---|---|---|
| Throughput | 10.9 FPS | Limited by serial execution |
| E2E latency | 87 ms (p50) | Lowest — no queue wait |
| Inference | 74 ms | 81% of pipeline time |

**Pros**: Lowest per-frame latency (no queue wait overhead). Simplest to debug.

**Cons**: Throughput capped by sum of all stages. While inference runs (~78ms), the camera, preprocessor, and network sit idle.

**Why it matters**: This is the baseline. All multi-threaded levels aim to overlap stages to improve throughput without proportionally increasing latency.

---

## Level 1: std::thread + Unbounded Queue

**File**: `device/src/pipeline_l1.cpp`

### Design

Three `std::thread` instances connected by unbounded `ThreadSafeQueue`:

```
recv thread ──[ThreadSafeQueue A]──▶ infer thread ──[ThreadSafeQueue B]──▶ upload thread
```

```cpp
// ThreadSafeQueue — unbounded, no capacity limit
template <typename T>
class ThreadSafeQueue {
    std::queue<T> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool shutdown_ = false;

public:
    void push(T item) {
        { std::lock_guard<std::mutex> lk(mtx_); queue_.push(std::move(item)); }
        cv_.notify_one();
    }

    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&]{ return !queue_.empty() || shutdown_; });
        if (queue_.empty()) return false;
        out = std::move(queue_.front()); queue_.pop();
        return true;
    }
};
```

### The Problem: Queue Grows Without Bound

When producer (recv) is faster than consumer (infer), frames accumulate:

```
Frames   11- 245:  E2E mean =  2.2 sec
Frames  246- 480:  E2E mean =  5.0 sec
Frames  481- 715:  E2E mean =  7.5 sec
Frames  716- 950:  E2E mean = 11.0 sec
Frames  951-1187:  E2E mean = 15.0 sec  ← linear growth, never converges
```

The recv thread captures at ~30 FPS (webcam) while infer processes at ~12.8 FPS. Every second, ~17 frames pile up in the queue. E2E latency grows linearly. Given enough time, this **will OOM**.

| Metric | Value | Note |
|---|---|---|
| Throughput | 12.8 FPS | Same as L2-L4 (inference-bound) |
| E2E latency | 7,551 ms (p50) | **86x worse than L0** |
| Queue wait | 8,004 ms mean | Frames wait seconds in queue |
| Jitter CV | 0.564 | Highly variable — latency depends on queue depth |

**Lesson**: An unbounded queue between a fast producer and slow consumer is a **ticking time bomb** in production. Throughput looks great, but latency degrades to seconds and memory grows until the process is killed.

---

## Level 2: POSIX pthread + Bounded Queue

**File**: `device/src/pipeline_l2.cpp`

### Design

Replaces STL threading with POSIX APIs and adds a **bounded capacity** (8 slots):

```cpp
// PosixQueue — bounded with POSIX semaphores
template <typename T>
class PosixQueue {
    std::queue<T> queue_;
    pthread_mutex_t mtx_;
    sem_t empty_count_;   // initialized to capacity (8)
    sem_t fill_count_;    // initialized to 0

public:
    void push(T item) {
        sem_wait(&empty_count_);    // block if queue is full
        pthread_mutex_lock(&mtx_);
        queue_.push(std::move(item));
        pthread_mutex_unlock(&mtx_);
        sem_post(&fill_count_);
    }

    bool pop(T& out) {
        sem_wait(&fill_count_);     // block if queue is empty
        pthread_mutex_lock(&mtx_);
        out = std::move(queue_.front()); queue_.pop();
        pthread_mutex_unlock(&mtx_);
        sem_post(&empty_count_);
        return true;
    }
};
```

### Backpressure

The `empty_count_` semaphore initialized to 8 means the producer can push at most 8 items before blocking. This prevents unbounded growth:

| Metric | L1 (unbounded) | L2 (bounded, cap=8) |
|---|---|---|
| Queue wait | 8,004 ms | 677 ms |
| E2E p50 | 7,551 ms | 790 ms |
| E2E p99 | 16,280 ms | 954 ms |

**93% reduction in queue wait time** compared to L1.

### Why POSIX over STL?

| Aspect | `std::mutex` + `condition_variable` | `pthread_mutex` + `sem_t` |
|---|---|---|
| Cross-process | No | Yes (`sem_init(pshared=1)`) |
| Priority inheritance | Platform-dependent | `PTHREAD_PRIO_INHERIT` |
| Deterministic timing | No guarantee | `sem_timedwait()` with `CLOCK_REALTIME` |
| Embedded relevance | Abstraction overhead | Direct POSIX API, available on all Unix |
| Interview signal | Standard C++ | Systems programming depth |

For this project, the practical difference is minimal. The choice demonstrates familiarity with POSIX primitives commonly used in embedded Linux (ChromeOS, Nest Hub).

---

## Level 3: RingBuffer + Pre-Allocated Slots (memcpy)

**File**: `device/src/pipeline_l3.cpp`

### Design

Replaces `std::queue` with a **fixed-size RingBuffer** and **pre-allocated slot structures**:

```cpp
// ring_buffer.h — SPSC RingBuffer with POSIX semaphores
template <typename T, int N>
class RingBuffer {
    T slots_[N];                    // slots live in the buffer itself
    sem_t empty_count_;             // tracks available write slots
    sem_t fill_count_;              // tracks available read slots
    pthread_mutex_t write_mtx_, read_mtx_;
    int write_idx_ = 0, read_idx_ = 0;

public:
    T* acquire_write_slot() {       // producer: get slot pointer
        sem_wait(&empty_count_);    // block if all slots full
        pthread_mutex_lock(&write_mtx_);
        int idx = write_idx_;
        write_idx_ = (write_idx_ + 1) % N;
        pthread_mutex_unlock(&write_mtx_);
        return &slots_[idx];
    }

    void commit_write_slot() { sem_post(&fill_count_); }

    T* acquire_read_slot() {        // consumer: get filled slot
        sem_wait(&fill_count_);     // block if no data
        pthread_mutex_lock(&read_mtx_);
        int idx = read_idx_;
        read_idx_ = (read_idx_ + 1) % N;
        pthread_mutex_unlock(&read_mtx_);
        return &slots_[idx];
    }

    void commit_read_slot() { sem_post(&empty_count_); }
};
```

### Pre-Allocated Slots

```cpp
// pipeline_slots.h
static constexpr int kMaxModelInputSize = 640 * 640 * 3;  // 1.2 MB per slot

struct InferSlot {
    uint8_t input_data[kMaxModelInputSize];   // pre-allocated model input buffer
    uint8_t orig_data[kOrigDataSize];         // pre-allocated original frame
    int     frame_number;
    int64_t ts_capture_start, ts_capture_end, ts_preprocess_end;
};
```

With `RingBuffer<InferSlot, 4>`, the 4 slots are allocated **once at startup** (4 x ~2 MB = ~8 MB). No per-frame heap allocation for the recv/infer path.

### The memcpy Issue (L3)

L3 still calls `resize()` which returns a `std::vector` (heap allocation), then copies into the slot:

```cpp
// L3 recv loop — still allocates a temporary vector
auto resized = preprocess::resize(frame, infer_w, infer_h);  // heap alloc!
memcpy(slot->input_data, resized.data(), resized.size());     // copy into slot
```

This is **almost** zero-malloc — the slot itself is pre-allocated, but `resize()` creates a temporary `std::vector` on every frame.

| Metric | L2 | L3 | Improvement |
|---|---|---|---|
| Queue wait | 677 ms | 220 ms | -67% |
| E2E p50 | 790 ms | 403 ms | -49% |
| E2E p99 | 954 ms | 506 ms | -47% |

The improvement comes from **fewer slots** (4 vs 8) providing stronger backpressure, not from eliminating malloc (which is negligible compared to 78ms inference).

---

## Level 4: Zero-Copy Production

**File**: `device/src/pipeline.cpp`

### Design

The final optimization: `resize_into()` writes **directly** into the pre-allocated slot buffer — no temporary allocation:

```cpp
// preprocess.h
void resize_into(const Frame& frame, int target_w, int target_h,
                 uint8_t* out_buf, int out_size);

// L4 recv loop — true zero-malloc
InferSlot* slot = ring_a.acquire_write_slot();
preprocess::resize_into(frame, infer_w, infer_h,
                        slot->input_data, kMaxModelInputSize);  // direct write!
```

The `resize_into()` implementation wraps the destination buffer with a `cv::Mat` header (no allocation) and resizes directly into it:

```cpp
void resize_into(const Frame& frame, int target_w, int target_h,
                 uint8_t* out_buf, int out_size) {
    cv::Mat src(frame.height, frame.width, CV_8UC3, const_cast<uint8_t*>(frame.data.data()));
    cv::Mat dst(target_h, target_w, CV_8UC3, out_buf);   // wraps existing buffer
    cv::resize(src, dst, cv::Size(target_w, target_h));   // writes directly
}
```

### L3 vs L4: Why Does Performance Barely Change?

| Metric | L3 | L4 | Diff |
|---|---|---|---|
| E2E mean | 395,782 us | 394,276 us | -0.4% |
| E2E p50 | 402,807 us | 399,972 us | -0.7% |
| Inference | 79,357 us | 78,332 us | -1.3% |

The difference is **< 1%** because inference (78ms) completely dominates the pipeline. The `resize()` temporary allocation in L3 costs ~1ms — negligible compared to 78ms inference.

### Why L4 Matters Despite Minimal Performance Gain

The engineering significance is not about benchmarks on this hardware:

1. **Steady-state zero-malloc** — recv and infer threads make **zero heap allocations** after startup. In embedded systems running for months, this prevents heap fragmentation that can cause unpredictable OOM.

2. **API design principle** — "Pre-allocated buffers are necessary but not sufficient. The API must also support in-place writes." L3 has the right data structure (pre-allocated slots) but the wrong API (`resize()` returns a vector). L4 fixes the API.

3. **Upload thread exception** — The upload thread still allocates for JPEG encoding (`cv::imencode`) and protobuf serialization. This is acceptable because: (a) upload is I/O-bound, not latency-critical, and (b) these allocations are bounded and short-lived.

### Steady-State Memory Profile (L4)

```
Thread          Heap Allocations in Steady State
────────        ─────────────────────────────────
recv            0  (resize_into writes directly into slot)
infer           0  (TFLite uses pre-allocated tensors, NMS result is small stack-like)
upload          ~50 KB/frame  (JPEG encode + protobuf, freed immediately)
```

---

## Pipeline Level Selection

Compile-time switch via CMake:

```bash
# docker-compose.yml passes PIPELINE_LEVEL as build arg
PIPELINE_LEVEL=0 docker compose build device   # Level 0
PIPELINE_LEVEL=2 docker compose build device   # Level 2
docker compose build device                     # Level 4 (default)
```

CMake maps level to source file:

```cmake
if(PIPELINE_LEVEL EQUAL 0)
    set(PIPELINE_SRC src/pipeline_l0.cpp)
elseif(PIPELINE_LEVEL EQUAL 1)
    set(PIPELINE_SRC src/pipeline_l1.cpp)
# ... etc
else()
    set(PIPELINE_SRC src/pipeline.cpp)  # L4 default
endif()
```

All levels implement the same `Pipeline` class interface — only the `.cpp` file changes:

```cpp
// pipeline.h — stable interface across all levels
class Pipeline {
public:
    int  start(const PipelineConfig& config);
    void stop();
    void request_shutdown();
private:
    struct Impl;
    Impl* impl_ = nullptr;
};
```

---

## Design Decision Q&A

### Q: Why not use lock-free queues?

The RingBuffer uses `sem_wait`/`sem_post` which involve kernel transitions. A truly lock-free SPSC queue (e.g., with `std::atomic` and memory ordering) would avoid this. However:
- Inference takes 78ms per frame. The semaphore overhead (~1us) is 0.001% of the pipeline.
- Semaphores provide clean blocking semantics — no busy-wait spin loops that waste CPU.
- POSIX semaphores are the standard primitive in embedded Linux.

### Q: Why 4 slots instead of 2 or 8?

Empirically measured:
- 2 slots: too tight, recv blocks on every frame → throughput drops
- 4 slots: optimal for this pipeline — allows recv to run 1-2 frames ahead without excessive latency
- 8 slots (L2): more burst tolerance, but queue wait increases from 220ms to 677ms

The sweet spot depends on the producer/consumer speed ratio. With `recv ~1ms` and `infer ~78ms`, 4 slots ≈ 4 * 78ms = ~312ms of buffering.

### Q: Why `pthread` instead of `std::thread`?

`std::thread` wraps pthread on Linux. The main reasons to use pthread directly:
- Access to thread attributes (`pthread_attr_t`): stack size, scheduling policy, CPU affinity
- Priority inheritance mutexes (`PTHREAD_PRIO_INHERIT`)
- Demonstrates POSIX systems knowledge relevant to ChromeOS/embedded Linux roles
- `std::thread` is used in L1 to contrast with L2+ POSIX approach

### Q: What about the `Frame::data` vector allocation in `next_frame()`?

The `Frame` struct's `data` field is a `std::vector<uint8_t>`. Each `next_frame()` call does:
```cpp
frame.data.assign(rgb.data, rgb.data + rgb.total() * rgb.elemSize());
```

This heap-allocates ~921 KB per frame in the recv thread. A fully zero-alloc system would use pre-allocated frame buffers in the HAL layer (e.g., `V4L2FrameSource` with mmap'd buffers). The current `FileFrameSource`/`OpenCVFrameSource` implementations prioritize simplicity.

### Q: How does shutdown work cleanly?

Signal handler sets `request_shutdown()` → calls `ring_a.shutdown()` + `ring_b.shutdown()`. The `shutdown()` method sets a flag and posts to both semaphores, unblocking any waiting thread. Each thread checks `is_shutdown()` in its loop condition and exits cleanly.

```cpp
void shutdown() {
    shutdown_ = true;
    sem_post(&empty_count_);  // unblock producer
    sem_post(&fill_count_);   // unblock consumer
}
```
