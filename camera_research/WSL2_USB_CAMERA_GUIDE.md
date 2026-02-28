# WSL2 USB Camera Complete Guide

## Environment Information

| Item | Value |
|---|---|
| OS | Windows 11 + WSL2 (Ubuntu) |
| Custom Kernel | `5.15.146.1-microsoft-standard-WSL2+` |
| Kernel bzImage Location | `C:\Users\427le\wsl-kernel\bzImage` |
| .wslconfig | `C:\Users\427le\.wslconfig` |
| usbipd-win Version | 5.3.0 |
| Tested Camera | AVerMedia Live Streamer CAM 310P (VID:PID `07ca:310b`) |
| Camera BUS-ID | `1-1` (may vary depending on USB port) |

---

## One-Time Setup (Already Completed)

The following steps only need to be done once. They are already completed and documented here for reference.

### 1. Install usbipd-win (Windows Side)

```powershell
# Admin PowerShell
winget install dorssel.usbipd-win
```

### 2. Build Custom WSL Kernel (Enable V4L2/UVC Drivers)

The default WSL kernel has `CONFIG_MEDIA_SUPPORT` set to `not set`, which prevents `/dev/video*` from appearing.
A custom kernel build is needed with the following config options enabled:

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

Build steps:

```bash
# Inside WSL
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
make -j$(nproc)    # ~30-60 minutes
```

### 3. Configure .wslconfig to Point to Custom Kernel

```ini
# C:\Users\427le\.wslconfig
[wsl2]
kernel=C:\\Users\\427le\\wsl-kernel\\bzImage
```

```bash
# Copy bzImage
cp arch/x86/boot/bzImage /mnt/c/Users/427le/wsl-kernel/
```

---

## Steps to Connect Camera (Each Time)

**These steps must be performed after each WSL restart or USB camera re-plug:**

### Step 1: Windows Side — Identify and Attach Camera

```powershell
# Admin PowerShell

# List USB devices, find camera BUS-ID
usbipd list

# Bind (needed on first use or after re-plug)
usbipd bind --busid <BUS-ID>

# Attach to WSL
usbipd attach --wsl --busid <BUS-ID>
```

> **Note**: BUS-ID may change depending on which USB port is used. Always run `usbipd list` first to confirm.
> Kaspersky antivirus may show a warning — this does not affect functionality. If bind fails, try adding `--force`.

### Step 2: WSL Side — Verify Device Appears

```bash
# Confirm USB device entered WSL
lsusb
# Should see: AVerMedia Technologies, Inc. Live Streamer CAM 310P

# Confirm video device appears
ls /dev/video*
# Should see: /dev/video0  /dev/video1
```

### Step 3: WSL Side — Set Device Permissions

```bash
# Default permissions are root-only, need to open read/write access
sudo chmod 666 /dev/video0 /dev/video1
```

> WSL does not have full udev support, so chmod must be done manually after each attach.

### Step 4: Test Capture

```python
import cv2

cap = cv2.VideoCapture("/dev/video0", cv2.CAP_V4L2)

# Key: Set MJPEG format (reduces USB bandwidth, avoids select() timeout)
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

### Step 5: Return Camera to Windows When Done

```powershell
# Admin PowerShell
usbipd detach --busid <BUS-ID>
```

---

## Quick Connection Checklist

Follow these steps each time:

```
□  1. [Windows Admin PowerShell] usbipd list           → Find BUS-ID
□  2. [Windows Admin PowerShell] usbipd bind --busid X
□  3. [Windows Admin PowerShell] usbipd attach --wsl --busid X
□  4. [WSL] lsusb                                       → Confirm camera appears
□  5. [WSL] ls /dev/video*                              → Confirm video device appears
□  6. [WSL] sudo chmod 666 /dev/video0 /dev/video1      → Set permissions
□  7. [WSL] Run Python test script                       → Capture frame
□  8. [Windows Admin PowerShell] usbipd detach --busid X → Return camera when done
```

---

## Known Issues and Solutions

### select() timeout

**Symptom**: Camera opens successfully but `cap.read()` keeps timing out.
**Cause**: Default YUYV format has insufficient bandwidth over usbipd USB-over-IP.
**Solution**: Force MJPEG format + lower resolution/FPS.

```python
cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
cap.set(cv2.CAP_PROP_FPS, 15)
```

### Permission denied

**Symptom**: `Cannot open video device /dev/video0: Permission denied`
**Cause**: WSL has no udev rules to automatically set video group permissions.
**Solution**: `sudo chmod 666 /dev/video0 /dev/video1`

### Laptop Built-in Camera (BUS-ID 1-9)

The laptop's built-in USB2.0 HD UVC WebCam can also be attached, but note:
- Once attached to WSL, the Windows side cannot use it
- BUS-ID is `1-9`, same procedure applies

---

## References

- [Microsoft Official: Connect USB Devices to WSL](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)
- [usbipd-win GitHub](https://github.com/dorssel/usbipd-win)
- [PINTO0309 WSL2 kernel USB camera config](https://github.com/PINTO0309/wsl2_linux_kernel_usbcam_enable_conf)
