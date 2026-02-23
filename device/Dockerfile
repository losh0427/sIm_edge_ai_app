FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential cmake git wget pkg-config \
    libopencv-dev \
    libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

# Clone TFLite source (cached layer)
RUN git clone --depth 1 --branch v2.14.0 \
    https://github.com/tensorflow/tensorflow.git /tf

# Pre-build TFLite (cached layer, ~15min first time)
RUN mkdir -p /tf/tflite_build && cd /tf/tflite_build && \
    cmake ../tensorflow/lite \
        -DCMAKE_BUILD_TYPE=Release \
        -DTFLITE_ENABLE_XNNPACK=ON && \
    cmake --build . -j$(nproc)

# Copy source
COPY proto/ /app/proto/
COPY device/ /app/device/

# Build edge agent
WORKDIR /app/device/build
ARG HAL_CAMERA_BACKEND=FILE
RUN cmake -DTFLITE_SOURCE_DIR=/tf -DCMAKE_BUILD_TYPE=Release \
          -DHAL_CAMERA_BACKEND=${HAL_CAMERA_BACKEND} .. && \
    cmake --build . -j$(nproc)

# Runtime stage
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    libopencv-core4.5d libopencv-imgproc4.5d libopencv-imgcodecs4.5d libopencv-videoio4.5d \
    libgrpc++1 libprotobuf23 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/device/build/edge_agent /usr/local/bin/
COPY models/ /app/models/
COPY data/ /app/data/

WORKDIR /app
CMD ["edge_agent"]
