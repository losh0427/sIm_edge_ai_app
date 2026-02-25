#!/usr/bin/env python3
"""Test gRPC client - validates server is working before C++ agent is ready."""
import grpc
import sys
import os
import time
import cv2
import numpy as np

# Generate proto stubs if not exist
if not os.path.exists("edge_ai_pb2.py"):
    os.system("python -m grpc_tools.protoc -I proto --python_out=. --grpc_python_out=. proto/edge_ai.proto")

import edge_ai_pb2
import edge_ai_pb2_grpc

def main():
    server = sys.argv[1] if len(sys.argv) > 1 else "localhost:50051"
    channel = grpc.insecure_channel(server)
    stub = edge_ai_pb2_grpc.EdgeServiceStub(channel)

    # Create dummy thumbnail (480x270 = 16:9, matching common webcam aspect ratio)
    img = np.random.randint(0, 255, (270, 480, 3), dtype=np.uint8)
    cv2.putText(img, "TEST", (180, 150), cv2.FONT_HERSHEY_SIMPLEX, 2, (0, 255, 0), 3)
    _, jpeg = cv2.imencode(".jpg", img)

    for i in range(100):
        req = edge_ai_pb2.DetectionFrame(
            edge_id="test-client",
            timestamp_ms=int(time.time() * 1000),
            thumbnail_jpeg=jpeg.tobytes(),
            inference_latency_ms=25.0 + np.random.randn() * 5,
            frame_number=i,
            hal_backend="TEST:dummy",
            original_width=640,
            original_height=480,
        )
        # Add fake detections
        box = req.boxes.add()
        box.x_min, box.y_min = 0.2, 0.3
        box.x_max, box.y_max = 0.6, 0.8
        box.class_id = 0  # person
        box.confidence = 0.85

        resp = stub.ReportDetection(req)
        print(f"Frame {i}: ok={resp.ok}")
        time.sleep(0.2)

if __name__ == "__main__":
    main()
