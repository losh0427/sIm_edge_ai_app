# RUNBOOK — Two-Machine Deployment

## Architecture

```
Machine A (Server)  192.168.0.195
  └─ Docker: sim_edge_ai_app-server
       ├─ :50051  gRPC receiver
       └─ :8501   Streamlit UI

Machine B (Device)  Local machine (WSL2)
  └─ Docker: sim_edge_ai_app-device
       ├─ /dev/video0  webcam via OpenCVFrameSource
       └─ → 192.168.0.195:50051  upstream gRPC
```

---

## Network Configuration (Server Side — Windows + WSL2)

Docker containers run inside WSL2, but external devices (Machine B) connect through Windows' Wi-Fi IP.
Two layers of configuration are needed: **Windows Firewall** + **port proxy (Windows → WSL2 forwarding)**.

### One-Time Setup: Firewall Rules

```powershell
# Admin PowerShell — only needed once, persists across reboots
New-NetFirewallRule -DisplayName "WSL2 gRPC 50051" `
  -Direction Inbound -Action Allow -Protocol TCP -LocalPort 50051

New-NetFirewallRule -DisplayName "WSL2 Streamlit 8501" `
  -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8501
```

Verify rules exist:
```powershell
Get-NetFirewallRule -DisplayName "WSL2*" | Format-Table DisplayName, Enabled
```

### Check After Each Reboot: Port Proxy

Port proxy rules themselves are **persistent** (survive reboots), but WSL2's internal IP **may change on each restart**.
If the IP changes, delete the old rules and recreate them.

```powershell
# Admin PowerShell

# 1. View current rules
netsh interface portproxy show v4tov4

# 2. Check current WSL IP
wsl -- hostname -I
# Example output: 172.21.49.121

# 3. If IP matches rules → no action needed
#    If IP changed → delete and recreate:
netsh interface portproxy delete v4tov4 listenport=50051 listenaddress=0.0.0.0
netsh interface portproxy delete v4tov4 listenport=8501  listenaddress=0.0.0.0

netsh interface portproxy add v4tov4 listenport=50051 listenaddress=0.0.0.0 connectport=50051 connectaddress=<WSL_IP>
netsh interface portproxy add v4tov4 listenport=8501  listenaddress=0.0.0.0 connectport=8501  connectaddress=<WSL_IP>
```

### Reboot Quick Checklist (Server Side)

```
□  1. [WSL]        wsl -- hostname -I                     → Note WSL IP
□  2. [PowerShell] netsh interface portproxy show v4tov4  → Compare IP
□  3. If IP differs → delete old rules + add new rules (commands above)
□  4. If IP matches → no action needed, start server directly
```

> **Why no need to recreate firewall rules?** `New-NetFirewallRule` writes to Windows registry, permanent.
> **Why port proxy might need updating?** WSL2 uses virtual NAT, IP is not fixed across restarts.

---

## Prerequisites: Download Model (both machines, first time only)

```bash
# Run from repo root
bash models/download_model.sh
# Expected output: Model downloaded: ~5.9M
```

---

## Machine A — Start Server

```bash
# From repo root
docker compose up server --build     # First time, needs build (~2 min)
docker compose up server             # Subsequent runs (image already exists)
```

Verify startup:
- gRPC: `nc -zv localhost 50051` → Connection succeeded
- UI: Open http://192.168.0.195:8501 in browser

Shutdown:
```bash
docker compose down
# Or Ctrl-C (if running in foreground)
```

---

## Machine B — Start Device (Local WSL2)

### 1. Verify Camera is Visible

```bash
ls /dev/video*
# /dev/video0 must exist before proceeding
```

### 2. Build Image (first time, ~15 min for TFLite compile)

```bash
docker build \
  --build-arg HAL_CAMERA_BACKEND=OPENCV \
  -t sim_edge_ai_app-device \
  -f device/Dockerfile \
  .
```

> If only `models/` or `data/` changed, only the last few layers are invalidated — rebuild takes seconds.
> If `device/src/` or `proto/` changed, rebuild starts from the compile layer (~3 min).

### 3. Run

```bash
docker run --rm \
  -e SERVER_ADDR=192.168.0.195:50051 \
  -e EDGE_ID=edge-2 \
  -e CONF_THRESH=0.4 \
  -e CAM_INDEX=0 \
  --device /dev/video0:/dev/video0 \
  sim_edge_ai_app-device:latest
```

Expected startup output:
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

Shutdown: `Ctrl-C` (container uses --rm so it is automatically removed)

---

## Environment Variables Reference

| Variable | Default | Description |
|---|---|---|
| `SERVER_ADDR` | `server:50051` | gRPC server address |
| `EDGE_ID` | `edge-1` | Device identifier name |
| `CONF_THRESH` | `0.4` | Detection confidence threshold |
| `CAM_INDEX` | `0` | Camera index `/dev/videoN` |
| `MODEL_PATH` | `/app/models/ssd_mobilenet_v2.tflite` | Model path (inside container) |
| `LABELS_PATH` | `/app/models/coco_labels.txt` | Labels path (inside container) |

---

## Common Issues

### `mmap ... failed with error '22'`
Model file does not exist or is 0 bytes.
```bash
ls -lh models/ssd_mobilenet_v2.tflite   # Verify ~5.9M
bash models/download_model.sh            # Re-download
docker build ...                         # Rebuild image
```

### `gRPC connected` then immediate disconnect
Server is not running, or firewall is blocking port 50051.
On Machine A verify: `docker compose up server` is running.

### `/dev/video0: No such file or directory`
WSL2 requires additional USB camera mounting (usbipd).
See `camera_research/WSL2_USB_CAMERA_GUIDE.md`.

### Forgot to add `HAL_CAMERA_BACKEND=OPENCV` build-arg
Image defaults to `FILE` mode, which tries to read `data/test_frames/` instead of the camera.
Rebuild with `--build-arg HAL_CAMERA_BACKEND=OPENCV`.

---

## Single Machine Testing (no camera needed)

```bash
# Terminal 1: Start server
docker compose up server

# Terminal 2: Use Python fake client to push test data
pip install grpcio grpcio-tools opencv-python numpy protobuf
python test_grpc_client.py localhost:50051

# Terminal 3: Use FILE mode device (reads test images)
pip install opencv-python numpy
python gen_test_frames.py              # Generate data/test_frames/
docker compose up device               # HAL_CAMERA_BACKEND=FILE
```
