#!/bin/bash
# ======================================================================
# Edge AI Benchmark: C++ device vs Python device
#
# 在 DEVICE 機器上執行。Server 需已在遠端啟動。
#
# 用法:
#   # 1. 遠端 server 機器先啟動:
#   #    docker compose up server --build
#
#   # 2. 本機(device)執行 benchmark:
#   #    FILE 模式 (本機 server):
#   bash benchmark.sh
#
#   #    兩機 + webcam:
#   SERVER_ADDR=192.168.0.195:50051 \
#   HAL_CAMERA_BACKEND=OPENCV \
#   MODEL_PATH=/app/models/yolov8n_float32.tflite \
#   LABELS_PATH=/app/models/coco80_labels.txt \
#   bash benchmark.sh
# ======================================================================
set -e

# --- Config ---
DURATION=${BENCH_DURATION:-60}          # seconds per run
WARMUP=${BENCH_WARMUP:-100}             # frames to skip before recording
RUNS=${BENCH_RUNS:-3}                   # repeat count
RESULTS_DIR="benchmark_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RUN_DIR="${RESULTS_DIR}/${TIMESTAMP}"

mkdir -p "$RUN_DIR"

cat <<EOF
======================================
  Edge AI Benchmark
======================================
  Server:     ${SERVER_ADDR:-server:50051}
  Model:      ${MODEL_PATH:-/app/models/ssd_mobilenet_v2.tflite}
  HAL:        ${HAL_CAMERA_BACKEND:-FILE}
  Duration:   ${DURATION}s per run
  Warmup:     ${WARMUP} frames
  Runs:       ${RUNS}x per agent (alternating)
  Output:     ${RUN_DIR}/
======================================

EOF

# --- Save benchmark config ---
cat > "${RUN_DIR}/config.txt" <<CONF
timestamp=$TIMESTAMP
server_addr=${SERVER_ADDR:-server:50051}
model_path=${MODEL_PATH:-/app/models/ssd_mobilenet_v2.tflite}
hal_backend=${HAL_CAMERA_BACKEND:-FILE}
conf_thresh=${CONF_THRESH:-0.4}
iou_thresh=${IOU_THRESH:-0.45}
duration_s=$DURATION
warmup_frames=$WARMUP
runs=$RUNS
CONF

# ======================================================================
# Helper: run one benchmark iteration
# ======================================================================
run_one() {
    local SERVICE=$1    # "device" or "py-device"
    local LABEL=$2      # "cpp" or "python"
    local RUN_NUM=$3    # 1, 2, 3...
    local PREFIX="${RUN_DIR}/${LABEL}_run${RUN_NUM}"
    local CSV_NAME="bench_${LABEL}_run${RUN_NUM}.csv"

    echo ""
    echo "--- ${LABEL} run #${RUN_NUM} ---"

    # Start device container with BENCH_CSV set
    BENCH_CSV="/app/data/${CSV_NAME}" \
    BENCH_WARMUP="${WARMUP}" \
    docker compose up "$SERVICE" -d --build 2>/dev/null

    sleep 3
    local CONTAINER
    CONTAINER=$(docker compose ps -q "$SERVICE" 2>/dev/null)

    if [ -z "$CONTAINER" ]; then
        echo "ERROR: container for $SERVICE not found"
        return 1
    fi

    echo "  Container: $CONTAINER"
    echo "  Running for ${DURATION}s (warmup=${WARMUP} frames)..."

    # Collect docker stats in background
    local STATS_FILE="${PREFIX}_docker_stats.csv"
    echo "timestamp,cpu_pct,mem_usage,mem_pct,net_io,pids" > "$STATS_FILE"
    (
        for i in $(seq 1 "$DURATION"); do
            docker stats "$CONTAINER" --no-stream --format \
                '{{.CPUPerc}},{{.MemUsage}},{{.MemPerc}},{{.NetIO}},{{.PIDs}}' 2>/dev/null | \
                while IFS= read -r line; do
                    echo "$(date +%s),$line"
                done >> "$STATS_FILE"
            sleep 1
        done
    ) &
    local STATS_PID=$!

    # Wait for duration
    sleep "$DURATION"

    # Stop stats collection
    kill "$STATS_PID" 2>/dev/null || true
    wait "$STATS_PID" 2>/dev/null || true

    # Save device logs
    docker compose logs "$SERVICE" --tail=200 > "${PREFIX}_device.log" 2>&1

    # Final snapshot
    docker stats "$CONTAINER" --no-stream > "${PREFIX}_final_stats.txt" 2>/dev/null

    # Stop device
    docker compose stop "$SERVICE" 2>/dev/null

    # Copy benchmark CSV from data/ volume
    if [ -f "data/${CSV_NAME}" ]; then
        cp "data/${CSV_NAME}" "${PREFIX}_latency.csv"
        echo "  Latency CSV: ${PREFIX}_latency.csv"
    else
        echo "  WARN: no latency CSV found (data/${CSV_NAME})"
    fi

    echo "  Docker stats: ${STATS_FILE}"
    echo "  Device log: ${PREFIX}_device.log"

    # Cool-down between runs
    echo "  Cooling down 10s..."
    sleep 10
}

# ======================================================================
# Main: alternating runs (C→P→C→P→C→P) for fairness
# ======================================================================
echo "Starting alternating benchmark runs..."

for i in $(seq 1 "$RUNS"); do
    run_one "device"    "cpp"    "$i"
    run_one "py-device" "python" "$i"
done

# ======================================================================
# Summary
# ======================================================================
echo ""
echo "======================================"
echo "  Benchmark Complete"
echo "======================================"
echo "  Results: ${RUN_DIR}/"
echo ""
ls -la "${RUN_DIR}/"
echo ""
echo "Per run:"
echo "  *_docker_stats.csv  — CPU%, Memory 每秒取樣"
echo "  *_latency.csv       — 分段延遲 CSV (per-frame)"
echo "  *_device.log        — FPS, latency log"
echo "  *_final_stats.txt   — 最終 docker stats"
echo ""
echo "Analyze:"
echo "  python benchmark_analyze.py ${RUN_DIR}"
