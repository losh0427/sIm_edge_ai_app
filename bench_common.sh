#!/bin/bash
# ======================================================================
# bench_common.sh — Shared functions for bench_cpp.sh / bench_python.sh
#
# Source this file, do NOT execute directly.
# ======================================================================

# --- Config (overridable via env) ---
WARMUP=${BENCH_WARMUP:-50}
RESULTS_DIR="benchmark_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# --- Globals (set by setup) ---
RUN_DIR=""
SERVICE=""
LABEL=""
CSV_NAME=""
STATS_PID=""

# ======================================================================
# setup SERVICE LABEL
#   e.g. setup "device" "cpp"  or  setup "py-device" "python"
# ======================================================================
setup() {
    SERVICE="$1"
    LABEL="$2"
    RUN_DIR="${RESULTS_DIR}/${LABEL}_${TIMESTAMP}"
    CSV_NAME="bench_${LABEL}.csv"

    mkdir -p "$RUN_DIR"

    cat <<EOF
========================================
  Edge AI Benchmark — ${LABEL^^} Device
========================================
  Server:     ${SERVER_ADDR:-server:50051}
  Model:      ${MODEL_PATH:-/app/models/ssd_mobilenet_v2.tflite}
  HAL:        ${HAL_CAMERA_BACKEND:-FILE}
  Warmup:     ${WARMUP} frames
  Output:     ${RUN_DIR}/
========================================

EOF

    # Save config
    cat > "${RUN_DIR}/config.txt" <<CONF
timestamp=$TIMESTAMP
agent=$LABEL
server_addr=${SERVER_ADDR:-server:50051}
model_path=${MODEL_PATH:-/app/models/ssd_mobilenet_v2.tflite}
hal_backend=${HAL_CAMERA_BACKEND:-FILE}
conf_thresh=${CONF_THRESH:-0.4}
iou_thresh=${IOU_THRESH:-0.45}
warmup_frames=$WARMUP
CONF
}

# ======================================================================
# ensure_server_up — Start server if not already running
# ======================================================================
ensure_server_up() {
    local server_status
    server_status=$(docker compose ps --status running server 2>/dev/null | tail -n +2)

    if [ -z "$server_status" ]; then
        echo "[*] Server not running. Starting server..."
        docker compose up server -d --build 2>/dev/null

        # Wait for gRPC port to be ready
        echo "[*] Waiting for server gRPC port (50051)..."
        local retries=0
        while ! docker compose exec -T server sh -c "ss -tlnp | grep -q 50051" 2>/dev/null; do
            retries=$((retries + 1))
            if [ "$retries" -ge 60 ]; then
                echo "ERROR: Server did not become ready within 60s"
                exit 1
            fi
            sleep 1
        done
        echo "[+] Server is ready."
    else
        echo "[+] Server already running, skipping."
    fi
}

# ======================================================================
# start_device — Launch the device container with BENCH_CSV env
# ======================================================================
start_device() {
    echo "[*] Starting ${LABEL} device (service: ${SERVICE})..."

    BENCH_CSV="/app/data/${CSV_NAME}" \
    BENCH_WARMUP="${WARMUP}" \
    docker compose up "$SERVICE" -d --build 2>/dev/null

    sleep 3
    local container
    container=$(docker compose ps -q "$SERVICE" 2>/dev/null)

    if [ -z "$container" ]; then
        echo "ERROR: container for ${SERVICE} not found"
        exit 1
    fi

    echo "[+] Container: $container"

    # Start background docker stats collection
    local stats_file="${RUN_DIR}/${LABEL}_docker_stats.csv"
    echo "timestamp,cpu_pct,mem_usage,mem_pct,net_io,pids" > "$stats_file"
    (
        while true; do
            docker stats "$container" --no-stream --format \
                '{{.CPUPerc}},{{.MemUsage}},{{.MemPerc}},{{.NetIO}},{{.PIDs}}' 2>/dev/null | \
                while IFS= read -r line; do
                    echo "$(date +%s),$line"
                done >> "$stats_file"
            sleep 1
        done
    ) &
    STATS_PID=$!
}

# ======================================================================
# wait_for_warmup — Wait until warmup frames are done
# ======================================================================
wait_for_warmup() {
    echo "[*] Waiting for warmup (${WARMUP} frames)..."

    # Poll the CSV file line count (subtract 1 for header)
    local csv_path="data/${CSV_NAME}"
    local retries=0
    while true; do
        if [ -f "$csv_path" ]; then
            local lines
            lines=$(wc -l < "$csv_path" 2>/dev/null || echo 0)
            lines=$((lines - 1))  # subtract header
            if [ "$lines" -ge "$WARMUP" ]; then
                break
            fi
        fi
        retries=$((retries + 1))
        if [ "$retries" -ge 600 ]; then
            echo "WARN: warmup timeout (600s). Proceeding anyway."
            break
        fi
        sleep 1
    done

    echo "[+] Warmup done (${WARMUP} frames)."
}

# ======================================================================
# stop_and_collect — Stop device, kill stats, copy results, analyze
# ======================================================================
stop_and_collect() {
    echo ""
    echo "[*] Stopping ${LABEL} device..."

    # Kill stats background process
    if [ -n "$STATS_PID" ]; then
        kill "$STATS_PID" 2>/dev/null || true
        wait "$STATS_PID" 2>/dev/null || true
        STATS_PID=""
    fi

    # Save device logs
    docker compose logs "$SERVICE" --tail=500 > "${RUN_DIR}/${LABEL}_device.log" 2>&1

    # Stop device container (5s grace period)
    docker compose stop -t 5 "$SERVICE" 2>/dev/null

    # Copy benchmark CSV from data/ volume
    if [ -f "data/${CSV_NAME}" ]; then
        cp "data/${CSV_NAME}" "${RUN_DIR}/${LABEL}_latency.csv"
        echo "[+] Latency CSV: ${RUN_DIR}/${LABEL}_latency.csv"
    else
        echo "[!] WARN: no latency CSV found (data/${CSV_NAME})"
    fi

    echo ""
    echo "========================================"
    echo "  Results saved to: ${RUN_DIR}/"
    echo "========================================"
    ls -la "${RUN_DIR}/"
    echo ""

    # Run analysis
    echo "[*] Running analysis..."
    python3 benchmark_analyze.py "${RUN_DIR}"
}

# ======================================================================
# cleanup — Called by trap on Ctrl+C
# ======================================================================
cleanup() {
    echo ""
    echo "[!] Ctrl+C received. Collecting results..."
    stop_and_collect
    exit 0
}
