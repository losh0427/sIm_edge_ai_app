# Pipeline Level 0-4 Benchmark Analysis

## 測試環境

- **Device**: WSL2 (webcam via OpenCVFrameSource)
- **Model**: YOLOv8n float32 (640x640, TFLite + XNNPACK)
- **Server**: 192.168.0.195:50051 (gRPC, 同網段)
- **Warmup**: 10 frames
- **每個 level 約跑 60 秒後 Ctrl+C 停止**

---

## 各 Level 設計差異

| Level | Threading | Queue / Buffer | Memory | 重點展示 |
|---|---|---|---|---|
| 0 | 單線程 | 無 | `std::vector` 每幀 heap alloc | 最簡單的 baseline |
| 1 | 3x `std::thread` | `std::queue` + mutex + cond_var（無上限） | `std::vector` 每幀 heap alloc | STL 多線程，展示 unbounded queue 問題 |
| 2 | 3x `pthread_create` | bounded queue + `sem_t`（capacity=8） | `std::vector` 每幀 heap alloc | POSIX API + backpressure |
| 3 | 3x `pthread_create` | `RingBuffer<T,4>` + pre-allocated slots | `resize()` → `memcpy` 到 slot | 接近 zero-malloc |
| 4 | 3x `pthread_create` | `RingBuffer<T,4>` + pre-allocated slots | `resize_into()` 直寫 slot | 真正 zero-copy production |

---

## 結果總覽

### Throughput（吞吐量）

| Level | Throughput FPS | 計算方式 |
|---|---|---|
| L0 | **10.9** | 1 / E2E_mean（串行，E2E = 全 pipeline 時間） |
| L1 | **~12.8** | 1 / inference_mean（並行，inference 是瓶頸） |
| L2 | **~12.8** | 同上 |
| L3 | **~12.6** | 同上 |
| L4 | **~12.8** | 同上 |

**結論**：所有多線程 level 的 throughput 幾乎相同（~12.8 FPS），因為**瓶頸永遠是 inference（~78ms/frame）**。多線程的好處是讓 capture、preprocess、thumbnail、gRPC 與 inference 並行執行，從 10.9 提升到 12.8 FPS（+17%）。

### Per-Frame Latency（單幀延遲）

| Metric | L0 | L1 | L2 | L3 | L4 |
|---|---|---|---|---|---|
| E2E mean | **92 ms** | 8,164 ms | 786 ms | 396 ms | **394 ms** |
| E2E p50 | **87 ms** | 7,551 ms | 790 ms | 403 ms | **400 ms** |
| E2E p90 | 105 ms | 15,193 ms | 840 ms | 446 ms | 441 ms |
| E2E p99 | 154 ms | 16,280 ms | 954 ms | 506 ms | 477 ms |
| Jitter CV | 0.166 | 0.564 | **0.081** | 0.160 | **0.148** |

> **注意**：L0 的 E2E 最低，因為沒有 queue wait。但 L0 是串行的，throughput 較低。
> 多線程 pipeline 的 E2E 包含 queue 等待時間，不能直接和 L0 比較。

### Stage Breakdown（mean, us）

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

> L3/L4 的 preprocess 包含 `acquire_write_slot()` 的阻塞時間（等 infer 消費完一個 slot），所以顯示 ~68ms 而非真正的 resize 成本（~1ms）。這正好反映了 backpressure 機制在運作。

---

## 關鍵發現

### 1. L1 Unbounded Queue — 記憶體持續增長

```
Frames   11- 245:  E2E mean =  2.2 sec
Frames  246- 480:  E2E mean =  5.0 sec
Frames  481- 715:  E2E mean =  7.5 sec
Frames  716- 950:  E2E mean = 11.0 sec
Frames  951-1187:  E2E mean = 15.0 sec  ← 線性增長，不收斂
```

recv thread 每幀 ~1ms（webcam 30fps），infer 每幀 ~78ms。每秒有 ~12 幀堆積在 queue 裡。跑越久，後面的 frame 等越久。如果不停止，最終會 OOM。

**教訓**：unbounded queue 在 producer > consumer 的場景下是一顆定時炸彈。

### 2. Bounded Queue (L2) vs RingBuffer (L3/L4) — Backpressure 強度

| | L2 (bounded, cap=8) | L3/L4 (RingBuffer, slots=4) |
|---|---|---|
| Queue wait mean | 677 ms | 220 ms |
| E2E p99 | 954 ms | 477-506 ms |

L2 的 capacity=8 允許更多 frame 在 queue 裡排隊，所以 queue wait 更長。L3/L4 只有 4 個 slot，backpressure 更強，recv 更頻繁地被阻塞，但每幀的 latency 更低。

**教訓**：更少的 buffer slots = 更強的 backpressure = 更低的 latency（犧牲的是 burst 容忍度）。

### 3. L3 vs L4 — 差異極小

| | L3 | L4 | 差異 |
|---|---|---|---|
| E2E mean | 395,782 us | 394,276 us | -0.4% |
| E2E p50 | 402,807 us | 399,972 us | -0.7% |
| Inference | 79,357 us | 78,332 us | -1.3% |

L3（`resize()` → `memcpy`）和 L4（`resize_into()` 直寫）在 throughput 和 latency 上幾乎無差異。因為 inference 的 78ms 完全主導了 pipeline，resize 的 1ms heap alloc 微不足道。

**但 L4 的意義在於**：
- steady state **零 malloc**（recv/infer thread），在嵌入式或長時間運行場景避免 heap fragmentation
- 展示「pre-allocated buffer 是必要條件，但 API 也要支援 in-place write 才能真正 zero-malloc」

### 4. Inference 是絕對瓶頸

所有 level 的 inference 都在 **74-79ms**，佔 L0 串行 pipeline 的 **81%**。

優化 inference 以外的 stage（preprocess 1ms、thumbnail 2-4ms、gRPC 11-13ms）對整體 throughput 的影響非常有限。真正要提升 FPS，需要：
- 更快的模型（量化 INT8、更小的架構）
- GPU/NPU 加速
- 模型 batch inference

---

## 總結

```
            Throughput    Latency     Memory Safety     Complexity
            (FPS)        (E2E p50)
  L0        10.9         87 ms       ✗ heap/frame      最簡單
  L1        12.8         7,551 ms    ✗ heap/frame      ✗ OOM risk
  L2        12.8         790 ms      ✗ heap/frame      中等
  L3        12.6         403 ms      △ 暫時 alloc      中高
  L4        12.8         400 ms      ✓ zero-malloc     最高

  瓶頸：inference ~78ms/frame（YOLOv8n float32 on CPU）
  多線程 throughput 提升：+17% (10.9 → 12.8 FPS)
```

**Pipeline level 的演進不是為了提升 throughput（那由 inference 決定），而是為了**：
1. **Latency 控制**：L2+ 的 bounded queue 防止 latency 無限增長
2. **Memory 安全**：L3/L4 的 pre-allocated buffer 消除 steady-state malloc
3. **Backpressure**：L3/L4 的 RingBuffer 在 producer > consumer 時自動限速
4. **Production readiness**：L4 的 zero-copy 在嵌入式/長時間運行環境中避免 heap fragmentation
