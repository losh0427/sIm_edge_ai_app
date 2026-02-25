#!/bin/bash
# ------------------------------------------------------------------
# Benchmark: C++ device vs Python device
#
# Prerequisites:
#   - models/ has the .tflite model file
#   - data/test_frames/ has test JPEGs
#   - docker compose images are built
#
# Usage:
#   bash benchmark.sh                         # default: SSD MobileNet, FILE
#   MODEL_PATH=/app/models/yolov8n_float32.tflite \
#   LABELS_PATH=/app/models/coco80_labels.txt \
#   bash benchmark.sh                         # YOLOv8n
# ------------------------------------------------------------------
set -e

DURATION=${BENCHMARK_DURATION:-30}  # seconds to sample docker stats
RESULTS_DIR="benchmark_results"
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

echo "=== Edge AI Benchmark ==="
echo "Duration: ${DURATION}s per test"
echo ""

# ------------------------------------------------------------------
# Helper: run a device service and collect stats
# ------------------------------------------------------------------
run_benchmark() {
    local SERVICE=$1
    local LABEL=$2
    local OUTFILE="${RESULTS_DIR}/${TIMESTAMP}_${LABEL}.txt"

    echo "--- Starting: $LABEL ---"

    # Start server + target device
    docker compose up -d server
    sleep 3  # wait for server to be ready
    docker compose up -d "$SERVICE"
    sleep 2  # wait for device to start inference

    local CONTAINER
    CONTAINER=$(docker compose ps -q "$SERVICE")

    if [ -z "$CONTAINER" ]; then
        echo "ERROR: container for $SERVICE not found"
        docker compose down
        return
    fi

    echo "Collecting stats for ${DURATION}s..."
    echo "# Benchmark: $LABEL  $(date)" > "$OUTFILE"
    echo "# Duration: ${DURATION}s" >> "$OUTFILE"
    echo "# Container: $CONTAINER" >> "$OUTFILE"
    echo "" >> "$OUTFILE"

    # Collect docker stats samples
    echo "timestamp,cpu_pct,mem_usage,mem_limit,mem_pct,net_io,pids" >> "$OUTFILE"
    for i in $(seq 1 "$DURATION"); do
        docker stats "$CONTAINER" --no-stream --format \
            "{{.CPUPerc}},{{.MemUsage}},{{.MemLimit}},{{.MemPerc}},{{.NetIO}},{{.PIDs}}" \
            >> "$OUTFILE" 2>/dev/null
        sleep 1
    done

    # Capture device logs (contains FPS / latency)
    echo "" >> "$OUTFILE"
    echo "# --- Device Logs ---" >> "$OUTFILE"
    docker compose logs "$SERVICE" --tail=50 >> "$OUTFILE" 2>&1

    # Get final memory RSS
    echo "" >> "$OUTFILE"
    echo "# --- Final Stats ---" >> "$OUTFILE"
    docker stats "$CONTAINER" --no-stream >> "$OUTFILE" 2>/dev/null

    echo "Stopping $SERVICE..."
    docker compose stop "$SERVICE"
    docker compose stop server
    sleep 2

    echo "Results saved: $OUTFILE"
    echo ""
}

# ------------------------------------------------------------------
# Run benchmarks
# ------------------------------------------------------------------

# 1. C++ device
run_benchmark "device" "cpp_device"

# 2. Python device
run_benchmark "py-device" "python_device"

# ------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------
echo "=== Benchmark Complete ==="
echo "Results in: $RESULTS_DIR/"
echo ""
echo "Files:"
ls -la "$RESULTS_DIR"/${TIMESTAMP}_*.txt
echo ""
echo "To compare, look at:"
echo "  - CPU %: average from the csv lines"
echo "  - Memory: MEM USAGE column"
echo "  - FPS: from device log output"
echo "  - Latency: from device log output"
