# WSL2 USB Camera 完整指南

## 環境資訊

| 項目 | 值 |
|---|---|
| OS | Windows 11 + WSL2 (Ubuntu) |
| Custom Kernel | `5.15.146.1-microsoft-standard-WSL2+` |
| Kernel bzImage 位置 | `C:\Users\427le\wsl-kernel\bzImage` |
| .wslconfig | `C:\Users\427le\.wslconfig` |
| usbipd-win 版本 | 5.3.0 |
| 測試成功的 Camera | AVerMedia Live Streamer CAM 310P (VID:PID `07ca:310b`) |
| Camera BUS-ID | `1-1`（可能因 USB 孔位而變） |

---

## 一次性設定（已完成）

以下步驟只需要做一次，已經完成，記錄在此供參考。

### 1. 安裝 usbipd-win（Windows 端）

```powershell
# 管理員 PowerShell
winget install dorssel.usbipd-win
```

### 2. 自建 WSL Kernel（啟用 V4L2/UVC 驅動）

預設 WSL kernel 的 `CONFIG_MEDIA_SUPPORT` 為 `not set`，無法產生 `/dev/video*`。
需要自建 kernel 啟用以下 config：

```
CONFIG_MEDIA_SUPPORT=y
CONFIG_MEDIA_USB_SUPPORT=y
CONFIG_MEDIA_CAMERA_SUPPORT=y
CONFIG_VIDEO_DEV=y
CONFIG_VIDEO_V4L2=y
CONFIG_USB_VIDEO_CLASS=y
CONFIG_VIDEOBUF2_CORE=y
CONFIG_VIDEOBUF2_V4L2=y
CONFIG_VIDEOBUF2_MEMOPS=y
CONFIG_VIDEOBUF2_VMALLOC=y
```

編譯步驟：

```bash
# WSL 內
sudo apt install -y build-essential flex bison libssl-dev libelf-dev bc dwarves python3

mkdir -p ~/wsl-kernel-build && cd ~/wsl-kernel-build
git clone --depth 1 --branch linux-msft-wsl-5.15.146.1 \
    https://github.com/microsoft/WSL2-Linux-Kernel.git
cd WSL2-Linux-Kernel

cp Microsoft/config-wsl .config
scripts/config --enable CONFIG_MEDIA_SUPPORT
scripts/config --enable CONFIG_MEDIA_USB_SUPPORT
scripts/config --enable CONFIG_MEDIA_CAMERA_SUPPORT
scripts/config --enable CONFIG_VIDEO_DEV
scripts/config --enable CONFIG_VIDEO_V4L2
scripts/config --enable CONFIG_USB_VIDEO_CLASS
scripts/config --enable CONFIG_VIDEOBUF2_CORE
scripts/config --enable CONFIG_VIDEOBUF2_V4L2
scripts/config --enable CONFIG_VIDEOBUF2_MEMOPS
scripts/config --enable CONFIG_VIDEOBUF2_VMALLOC
scripts/config --enable CONFIG_USB_ANNOUNCE_NEW_DEVICES
make olddefconfig
make -j$(nproc)    # 約 30-60 分鐘
```

### 3. 設定 .wslconfig 指向自建 kernel

```ini
# C:\Users\427le\.wslconfig
[wsl2]
kernel=C:\\Users\\427le\\wsl-kernel\\bzImage
```

```bash
# 複製 bzImage
cp arch/x86/boot/bzImage /mnt/c/Users/427le/wsl-kernel/
```

---

## 每次連接 Camera 的步驟

**每次重啟 WSL 或重新插拔 USB camera 後，都需要執行以下步驟：**

### Step 1: Windows 端 — 確認並 attach camera

```powershell
# 管理員 PowerShell

# 列出 USB 裝置，找到 camera 的 BUS-ID
usbipd list

# Bind（首次或重新插拔後需要）
usbipd bind --busid <BUS-ID>

# Attach 到 WSL
usbipd attach --wsl --busid <BUS-ID>
```

> **注意**：BUS-ID 可能因 USB 孔位不同而改變，每次都要先 `usbipd list` 確認。
> Kaspersky 防毒可能會出 warning，不影響功能。如果 bind 失敗可以加 `--force`。

### Step 2: WSL 端 — 確認裝置出現

```bash
# 確認 USB 裝置已進入 WSL
lsusb
# 應看到: AVerMedia Technologies, Inc. Live Streamer CAM 310P

# 確認 video device 出現
ls /dev/video*
# 應看到: /dev/video0  /dev/video1
```

### Step 3: WSL 端 — 修改裝置權限

```bash
# 預設權限是 root only，需要開放讀寫
sudo chmod 666 /dev/video0 /dev/video1
```

> WSL 沒有完整的 udev，所以每次 attach 後都要手動 chmod。

### Step 4: 測試擷取

```python
import cv2

cap = cv2.VideoCapture("/dev/video0", cv2.CAP_V4L2)

# 關鍵：設定 MJPEG 格式（減少 USB 頻寬，避免 select() timeout）
fourcc = cv2.VideoWriter_fourcc(*'MJPG')
cap.set(cv2.CAP_PROP_FOURCC, fourcc)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
cap.set(cv2.CAP_PROP_FPS, 15)
cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

ret, frame = cap.read()
if ret:
    print(f"OK: {frame.shape}")
    cv2.imwrite("test.jpg", frame)
cap.release()
```

### Step 5: 用完後歸還 camera 給 Windows

```powershell
# 管理員 PowerShell
usbipd detach --busid <BUS-ID>
```

---

## 快速連接 Checklist

每次使用時照著做：

```
□  1. [Windows PowerShell 管理員] usbipd list           → 找到 BUS-ID
□  2. [Windows PowerShell 管理員] usbipd bind --busid X
□  3. [Windows PowerShell 管理員] usbipd attach --wsl --busid X
□  4. [WSL] lsusb                                       → 確認 camera 出現
□  5. [WSL] ls /dev/video*                              → 確認 video device 出現
□  6. [WSL] sudo chmod 666 /dev/video0 /dev/video1      → 開放權限
□  7. [WSL] python3 測試程式                              → 擷取畫面
□  8. [Windows PowerShell 管理員] usbipd detach --busid X → 用完歸還
```

---

## 已知問題與解決方案

### select() timeout

**症狀**：Camera 能開啟但 `cap.read()` 一直 timeout。
**原因**：預設 YUYV 格式在 usbipd USB-over-IP 下頻寬不足。
**解法**：強制使用 MJPEG 格式 + 降低解析度/FPS。

```python
cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
cap.set(cv2.CAP_PROP_FPS, 15)
```

### Permission denied

**症狀**：`Cannot open video device /dev/video0: Permission denied`
**原因**：WSL 沒有 udev rule 自動設定 video group 權限。
**解法**：`sudo chmod 666 /dev/video0 /dev/video1`

### 筆電內建鏡頭 (BUS-ID 1-9)

筆電內建的 USB2.0 HD UVC WebCam 也可以 attach，但需要注意：
- 內建鏡頭 attach 到 WSL 後 Windows 端無法使用
- BUS-ID 是 `1-9`，操作同上

---

## 參考資源

- [Microsoft 官方: WSL 連接 USB 裝置](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)
- [usbipd-win GitHub](https://github.com/dorssel/usbipd-win)
- [PINTO0309 WSL2 kernel USB camera config](https://github.com/PINTO0309/wsl2_linux_kernel_usbcam_enable_conf)
