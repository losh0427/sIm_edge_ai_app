#!/bin/bash
# ======================================================================
# bench_python.sh — Python Device Benchmark
#
# Usage:
#   bash bench_python.sh
#
# Starts server (if needed) + Python device, collects data until Ctrl+C.
# ======================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/bench_common.sh"

setup "py-device" "python"
ensure_server_up

trap cleanup SIGINT SIGTERM

start_device
wait_for_warmup

echo ""
echo "========================================"
echo "  Data collection in progress..."
echo "  Press Ctrl+C to stop and analyze"
echo "========================================"
echo ""

# Block until user presses Ctrl+C
while true; do sleep 1; done
