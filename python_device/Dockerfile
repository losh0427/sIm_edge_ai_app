# Python baseline device agent — for benchmarking against C++ implementation
FROM python:3.11-slim

RUN apt-get update && apt-get install -y \
    libgl1 libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY python_device/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Generate proto stubs
COPY proto/ /app/proto/
RUN python -m grpc_tools.protoc \
    -I /app/proto \
    --python_out=/app \
    --grpc_python_out=/app \
    /app/proto/edge_ai.proto

COPY python_device/ /app/

CMD ["python", "main.py"]
