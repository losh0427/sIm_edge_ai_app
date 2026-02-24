# RUNBOOK — Two-Machine Deployment

## 架構

```
Machine A (Server)  192.168.0.195
  └─ Docker: sim_edge_ai_app-server
       ├─ :50051  gRPC receiver
       └─ :8501   Streamlit UI

Machine B (Device)  本機 (WSL2)
  └─ Docker: sim_edge_ai_app-device
       ├─ /dev/video0  webcam via OpenCVFrameSource
       └─ → 192.168.0.195:50051  upstream gRPC
```

---

## 前置：下載模型（兩台都要，第一次才做）

```bash
# 從 repo 根目錄執行
bash models/download_model.sh
# 預期輸出: Model downloaded: ~5.9M
```

---

## Machine A — 啟動 Server

```bash
# 從 repo 根目錄
docker compose up server --build     # 第一次，需 build（約 2 min）
docker compose up server             # 之後（image 已存在）
```

確認啟動：
- gRPC : `nc -zv localhost 50051` → Connection succeeded
- UI   : 瀏覽器開 http://192.168.0.195:8501

關閉：
```bash
docker compose down
# 或 Ctrl-C（若是前景執行）
```

---

## Machine B — 啟動 Device（本機 WSL2）

### 1. 確認攝影機可見

```bash
ls /dev/video*
# /dev/video0 存在才能繼續
```

### 2. Build image（第一次，約 15 min，TFLite compile）

```bash
docker build \
  --build-arg HAL_CAMERA_BACKEND=OPENCV \
  -t sim_edge_ai_app-device \
  -f device/Dockerfile \
  .
```

> 後續若只改了 models/ 或 data/，只有最後幾層失效，rebuild 幾秒完成。
> 若改了 device/src/ 或 proto/，則從 compile 層開始重跑（~3 min）。

### 3. 啟動

```bash
docker run --rm \
  -e SERVER_ADDR=192.168.0.195:50051 \
  -e EDGE_ID=edge-2 \
  -e CONF_THRESH=0.4 \
  -e CAM_INDEX=0 \
  --device /dev/video0:/dev/video0 \
  sim_edge_ai_app-device:latest
```

正常啟動輸出：
```
Loaded 80 labels
OpenCVFrameSource: opened /dev/video0 (640x480 MJPEG)
Frame source: OPENCV:cam0:640x480
INFO: Created TensorFlow Lite XNNPACK delegate for CPU.
Model loaded: input 300x300
gRPC connected to 192.168.0.195:50051
[upload] thread started
[infer] thread started
Pipeline: 3 threads started (recv → infer → upload)
[recv] thread started
[upload] Frame 1 | 2.0 FPS | 157.9 ms | 0 detections
```

關閉：`Ctrl-C`（容器 --rm 所以會自動刪除）

---

## 環境變數參考

| 變數 | 預設 | 說明 |
|---|---|---|
| `SERVER_ADDR` | `server:50051` | gRPC server 位址 |
| `EDGE_ID` | `edge-1` | 裝置識別名稱 |
| `CONF_THRESH` | `0.4` | 偵測信心門檻 |
| `CAM_INDEX` | `0` | 攝影機編號 `/dev/videoN` |
| `MODEL_PATH` | `/app/models/ssd_mobilenet_v2.tflite` | 模型路徑（容器內） |
| `LABELS_PATH` | `/app/models/coco_labels.txt` | 標籤路徑（容器內） |

---

## 常見問題

### `mmap ... failed with error '22'`
模型檔案不存在或為 0 bytes。
```bash
ls -lh models/ssd_mobilenet_v2.tflite   # 確認 ~5.9M
bash models/download_model.sh            # 重新下載
docker build ...                         # 重建 image
```

### `gRPC connected` 後馬上斷線
Server 尚未啟動，或防火牆擋住 50051。
Machine A 確認：`docker compose up server` 已在跑。

### `/dev/video0: No such file or directory`
WSL2 需要額外掛載 USB 攝影機（usbipd）。
參考 `camera_research/WSL2_USB_CAMERA_GUIDE.md`。

### build-arg 忘記加 `HAL_CAMERA_BACKEND=OPENCV`
image 預設為 `FILE` 模式，會嘗試讀 `data/test_frames/` 而非攝影機。
重新 build 時加上 `--build-arg HAL_CAMERA_BACKEND=OPENCV`。

---

## 單機測試（不需要攝影機）

```bash
# Terminal 1：啟動 server
docker compose up server

# Terminal 2：用 Python fake client 推假資料
pip install grpcio grpcio-tools opencv-python numpy protobuf
python test_grpc_client.py localhost:50051

# Terminal 3：用 FILE 模式 device（讀測試圖）
pip install opencv-python numpy
python gen_test_frames.py              # 產生 data/test_frames/
docker compose up device               # HAL_CAMERA_BACKEND=FILE
```
