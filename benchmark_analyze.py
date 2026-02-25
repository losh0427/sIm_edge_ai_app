#!/usr/bin/env python3
"""
Analyze benchmark results and generate summary + charts.

Usage:
    python benchmark_analyze.py benchmark_results/<timestamp>/
"""
import sys
import os
import csv
import re
import statistics

def parse_docker_stats(path):
    """Parse docker_stats.csv → list of {cpu_pct, mem_mb}."""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        reader = csv.reader(f)
        next(reader)  # skip header
        for row in reader:
            if len(row) < 3:
                continue
            try:
                cpu = float(row[1].replace("%", ""))
                # mem_usage format: "81.05MiB / 7.61GiB" — take first part
                mem_str = row[2].split("/")[0].strip()
                if "GiB" in mem_str:
                    mem_mb = float(mem_str.replace("GiB", "")) * 1024
                elif "MiB" in mem_str:
                    mem_mb = float(mem_str.replace("MiB", ""))
                elif "KiB" in mem_str:
                    mem_mb = float(mem_str.replace("KiB", "")) / 1024
                else:
                    mem_mb = 0
                rows.append({"cpu_pct": cpu, "mem_mb": mem_mb})
            except (ValueError, IndexError):
                continue
    return rows

def parse_device_log_fps(path):
    """Extract FPS values from device log."""
    fps_values = []
    if not os.path.exists(path):
        return fps_values
    with open(path) as f:
        for line in f:
            # Match patterns like "4.7 FPS" or "12.3 FPS"
            m = re.search(r'([\d.]+)\s*FPS', line)
            if m:
                fps_values.append(float(m.group(1)))
    return fps_values

def parse_latency_csv(path):
    """Parse benchmark latency CSV (works for both C++ and Python output)."""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            parsed = {}
            for k, v in row.items():
                try:
                    parsed[k] = int(v)
                except (ValueError, TypeError):
                    parsed[k] = 0
            rows.append(parsed)
    return rows

def percentile(data, p):
    if not data:
        return 0
    sorted_data = sorted(data)
    idx = int(len(sorted_data) * p / 100)
    idx = min(idx, len(sorted_data) - 1)
    return sorted_data[idx]

def summarize_stats(label, docker_stats, fps_values, latency_rows=None):
    """Print summary for one agent type."""
    print(f"\n{'='*50}")
    print(f"  {label}")
    print(f"{'='*50}")

    if docker_stats:
        cpus = [s["cpu_pct"] for s in docker_stats]
        mems = [s["mem_mb"] for s in docker_stats]
        print(f"  CPU %:     mean={statistics.mean(cpus):.1f}  "
              f"median={statistics.median(cpus):.1f}  "
              f"max={max(cpus):.1f}")
        print(f"  Memory:    mean={statistics.mean(mems):.1f} MB  "
              f"max={max(mems):.1f} MB")

    if fps_values:
        # Take last value as steady-state FPS
        steady_fps = fps_values[-1] if fps_values else 0
        print(f"  FPS:       steady={steady_fps:.1f}  "
              f"max={max(fps_values):.1f}")

    if latency_rows:
        e2e = [r["e2e_us"] for r in latency_rows]
        infer = [r["inference_us"] for r in latency_rows]
        preproc = [r["preprocess_us"] for r in latency_rows]
        nms = [r["nms_us"] for r in latency_rows]
        thumb = [r["thumbnail_us"] for r in latency_rows]
        grpc = [r["grpc_us"] for r in latency_rows]
        capture = [r.get("capture_us", 0) for r in latency_rows]

        print(f"\n  Latency breakdown (us):")
        print(f"  {'Stage':<15} {'mean':>8} {'p50':>8} {'p90':>8} {'p99':>8} {'max':>8}")
        print(f"  {'-'*55}")
        for name, data in [("capture", capture), ("preprocess", preproc),
                           ("inference", infer), ("nms", nms),
                           ("thumbnail", thumb), ("grpc_send", grpc),
                           ("E2E", e2e)]:
            if not data or all(v == 0 for v in data):
                continue
            print(f"  {name:<15} {statistics.mean(data):>8.0f} "
                  f"{percentile(data, 50):>8} "
                  f"{percentile(data, 90):>8} "
                  f"{percentile(data, 99):>8} "
                  f"{max(data):>8}")

        # RSS only in Python CSV
        rss = [r.get("rss_kb", 0) for r in latency_rows]
        if any(v > 0 for v in rss):
            print(f"\n  RSS: mean={statistics.mean(rss):.0f} KB  "
                  f"max={max(rss)} KB")

        # Jitter
        if len(e2e) > 1:
            cv = statistics.stdev(e2e) / statistics.mean(e2e)
            tail_ratio = percentile(e2e, 99) / max(percentile(e2e, 50), 1)
            print(f"  Jitter:    CV={cv:.3f}  p99/p50={tail_ratio:.2f}")

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <benchmark_results_dir>")
        sys.exit(1)

    run_dir = sys.argv[1]
    if not os.path.isdir(run_dir):
        print(f"Directory not found: {run_dir}")
        sys.exit(1)

    print(f"Analyzing: {run_dir}")

    # Find all runs
    for agent in ["cpp", "python"]:
        all_docker_stats = []
        all_fps = []
        all_latency = []

        run_num = 1
        while True:
            prefix = os.path.join(run_dir, f"{agent}_run{run_num}")
            stats_file = f"{prefix}_docker_stats.csv"
            log_file = f"{prefix}_device.log"
            latency_file = f"{prefix}_latency.csv"

            if not os.path.exists(stats_file) and not os.path.exists(log_file):
                break

            ds = parse_docker_stats(stats_file)
            fps = parse_device_log_fps(log_file)
            lat = parse_latency_csv(latency_file)

            all_docker_stats.extend(ds)
            all_fps.extend(fps)
            all_latency.extend(lat)
            run_num += 1

        if all_docker_stats or all_fps or all_latency:
            label = "C++ Device" if agent == "cpp" else "Python Device"
            summarize_stats(label, all_docker_stats, all_fps,
                            all_latency if all_latency else None)

    # Comparison table
    print(f"\n{'='*50}")
    print(f"  Comparison Summary")
    print(f"{'='*50}")
    print(f"  (See individual sections above for detailed metrics)")
    print(f"  Key areas where C++ should win:")
    print(f"    - E2E latency (pipeline parallelism)")
    print(f"    - Tail latency stability (no GC)")
    print(f"    - Memory usage (zero-malloc)")
    print(f"    - FPS throughput (multi-thread)")

if __name__ == "__main__":
    main()
