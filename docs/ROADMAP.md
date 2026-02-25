# Project Roadmap

## 目標

展現 C/C++、嵌入式、虛擬環境、資源部署、ML inference、OS、socket programming 等技術能力。
透過 Python baseline 對比，量化 C++ 實作的工程優勢。

---

## Phase 1: Python Baseline + Benchmark（1-2 天）

建立等價的 Python device agent，與 C++ 版本在相同條件下 benchmark。

### 產出
- `python_device/main.py` — 單線程 TFLite Python pipeline（讀圖 → 推論 → gRPC 送出）
- `python_device/Dockerfile` — python:3.11-slim + tflite-runtime
- `bench_cpp.sh` / `bench_python.sh` — 獨立 benchmark 腳本（Ctrl+C 停止 + 自動分析）
- `docs/benchmark.md` — 數據表 + 分析

### 衡量指標
| 指標 | 量測方式 | 預期 C++ 優勢 |
|---|---|---|
| Inference throughput (FPS) | 同機器、同 model，跑 100 幀取平均 | 1.5-3x |
| End-to-end latency (ms/frame) | 讀取影像到 gRPC 送出 | 2-5x |
| Memory RSS (MB) | `docker stats` 穩態記憶體 | C++ 30-50MB vs Python 200MB+ |
| CPU usage (%) | `docker stats` 平均 CPU | C++ 更低（zero-malloc + 無 GC） |
| Startup time (s) | container 啟動到第一幀送出 | C++ 快（無 Python import chain） |
| Steady-state allocation | heap 增長觀察 | C++ = 0 malloc |

### Benchmark 初步結果（SSD MobileNet v2, FILE mode, 同機器）

| 指標 | C++ | Python | 備註 |
|---|---|---|---|
| FPS (steady) | 9.3 | 5.9 | C++ throughput 1.58x |
| E2E latency mean | 536 ms | 144 ms | Python 單線程無排隊延遲 |
| E2E latency p99 | 676 ms | 231 ms | |
| Memory (mean) | 50 MB | 58 MB | C++ 略優 |
| CPU % (mean) | 377% | 173% | C++ 3-thread 用了 ~4 核 |
| Jitter CV | 0.087 | 0.107 | C++ 更穩定 |
| Tail ratio p99/p50 | 1.27x | 1.65x | C++ tail latency 更好 |

**觀察**：C++ FPS 更高但 E2E 更慢，原因是 3-thread pipeline 的 ring buffer 排隊延遲。另外 C++ preprocess 異常慢（75ms vs Python 0.5ms），值得調查。

### 待優化項目
- [ ] 調查 C++ preprocess 瓶頸（OpenCV resize 在 pipeline 中的等待時間）
- [ ] 考慮 E2E latency 計算方式是否包含了 ring buffer 等待時間，需區分 pipeline latency vs per-stage latency
- [ ] 嘗試 YOLOv8n model 的 benchmark 比較

---

## Phase 2: C++ Pipeline Levels（2-3 天）

拆出多個實作等級，展示從 naive 到 optimized 的演進過程。

### Pipeline Levels
| Level | 內容 | 練習點 |
|---|---|---|
| Level 0 | 單線程、std::vector 動態分配、std::thread | 基本能跑 |
| Level 1 | 3 線程，std::queue + std::mutex | 多線程基礎 |
| Level 2 | POSIX pthread_mutex + sem_t，動態分配 | POSIX API |
| Level 3 | Pre-allocated RingBuffer，zero-malloc | 記憶體管理 |
| Level 4（現行）| RingBuffer + POSIX sem + zero-malloc + 手寫 NMS | 完整版 |

### 實作方式
- CMake flag 切換：`-DPIPELINE_LEVEL=0|1|2|3|4`
- 每個 level 獨立檔案：`pipeline_l0.cpp` ~ `pipeline_l4.cpp`
- 每個 level 跑 benchmark 記錄演進數據

### 產出
- `device/src/pipeline_l0.cpp` ~ `pipeline_l3.cpp`
- CMakeLists.txt 更新
- `docs/cpp_deep_dive.md` — 每層優化的「為什麼」

### cpp_deep_dive.md 重點章節
| 主題 | 說明 |
|---|---|
| Zero-malloc steady state | 嵌入式不能有 heap fragmentation，GC pause 不可接受 |
| POSIX sem + mutex vs std::mutex | 跨 process、priority inheritance、更貼近 kernel |
| RingBuffer SPSC | lock-free 可能性、cache line 友善、bounded memory |
| 手寫 NMS | 不依賴框架、可控制記憶體、可 SIMD 優化 |
| HAL 抽象層 | 硬體替換不動推論邏輯、compile-time 零 runtime 開銷 |
| 3-thread pipeline | pipeline parallelism、throughput vs latency 取捨 |

---

## Phase 3: Cross-Platform ARM/x86（1 天）

不需要實體 ARM 硬體，證明跨平台能力。

### 方案 A：Docker multi-arch build
```bash
docker buildx build --platform linux/amd64,linux/arm64 -t device .
```

### 方案 B：CMake cross-compile toolchain
```cmake
# toolchain-aarch64.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
```

### 產出
- `toolchain-aarch64.cmake`
- Docker buildx 設定
- `docs/cross_platform.md`

---

## Phase 4: K3s 部署（bonus，1-2 天）

展示 container orchestration 能力。

### 產出
- K3s manifest 或 Helm chart
- server + device 作為 pod 部署
- 部署文件

---

## Phase 5: 技術文件收尾

### 產出
- `docs/architecture.md` — 整體架構圖 + 設計理由
- `docs/benchmark.md` — Python vs C++ 完整數據
- `docs/cpp_deep_dive.md` — C++ 優化點解釋
- `docs/cross_platform.md` — ARM/x86 build 說明
- README 更新，加入 benchmark 結果摘要
