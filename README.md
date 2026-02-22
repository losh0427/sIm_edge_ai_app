# Edge AI Runtime

Real-time object detection on simulated IoT edge devices.

## Quick Start (Single Machine, Docker)

```bash
# 1. Download model
bash models/download_model.sh

# 2. Generate test frames
pip install opencv-python numpy
python gen_test_frames.py

# 3. Build & run
docker compose up --build

# 4. Open Streamlit UI
# http://localhost:8501
```

## Test Server Only (without C++ agent)

```bash
# Terminal 1: Start server
docker compose up server

# Terminal 2: Run Python test client
pip install grpcio grpcio-tools opencv-python numpy protobuf
python test_grpc_client.py localhost:50051
```

## Project Structure

```
proto/          gRPC protobuf definition
device/         C++ edge agent (TFLite + NMS + gRPC client)
  hal/          Hardware Abstraction Layer (FileSource, V4L2, OpenCV)
  src/          Core pipeline (preprocess, inference, postprocess, grpc)
server/         Python server (gRPC receiver + Streamlit UI)
models/         TFLite models + labels
data/           Test images
```
