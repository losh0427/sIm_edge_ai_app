FROM python:3.11-slim

RUN apt-get update && apt-get install -y \
    libgl1-mesa-glx libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY server/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Generate proto stubs into server dir so imports work
COPY proto/ /app/proto/
COPY server/ /app/server/
COPY models/coco_labels.txt /app/models/coco_labels.txt

RUN python -m grpc_tools.protoc \
    -I /app/proto \
    --python_out=/app/server \
    --grpc_python_out=/app/server \
    /app/proto/edge_ai.proto

WORKDIR /app/server
EXPOSE 50051 8501

CMD ["streamlit", "run", "app/main.py", "--server.port=8501", "--server.address=0.0.0.0", "--server.headless=true"]
