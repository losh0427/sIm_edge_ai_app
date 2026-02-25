#!/usr/bin/env python3
"""
Analyze benchmark results and generate summary + comparison.

Usage:
    # Single directory (one agent):
    python benchmark_analyze.py benchmark_results/cpp_20260225_120000/

    # Dual directory (comparison):
    python benchmark_analyze.py benchmark_results/cpp_20260225_120000/ benchmark_results/python_20260225_130000/
"""
import sys
import os
import csv
import re
import statistics


def parse_docker_stats(path):
    """Parse docker_stats.csv -> list of {cpu_pct, mem_mb}."""
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


def detect_agent(run_dir):
    """Detect agent type from directory name or config file."""
    dirname = os.path.basename(run_dir.rstrip("/"))
    if dirname.startswith("cpp"):
        return "cpp"
    if dirname.startswith("python"):
        return "python"
    # Fallback: check config.txt
    config_path = os.path.join(run_dir, "config.txt")
    if os.path.exists(config_path):
        with open(config_path) as f:
            for line in f:
                if line.startswith("agent="):
                    return line.strip().split("=", 1)[1]
    return "unknown"


def load_run_data(run_dir):
    """Load all data from a single run directory."""
    agent = detect_agent(run_dir)
    label = "C++ Device" if agent == "cpp" else "Python Device"

    # Try new naming: {agent}_docker_stats.csv, {agent}_latency.csv, {agent}_device.log
    stats_file = os.path.join(run_dir, f"{agent}_docker_stats.csv")
    log_file = os.path.join(run_dir, f"{agent}_device.log")
    latency_file = os.path.join(run_dir, f"{agent}_latency.csv")

    docker_stats = parse_docker_stats(stats_file)
    fps_values = parse_device_log_fps(log_file)
    latency_rows = parse_latency_csv(latency_file)

    # Fallback: try old run{N} naming pattern
    if not docker_stats and not fps_values and not latency_rows:
        run_num = 1
        while True:
            prefix = os.path.join(run_dir, f"{agent}_run{run_num}")
            sf = f"{prefix}_docker_stats.csv"
            lf = f"{prefix}_device.log"
            laf = f"{prefix}_latency.csv"
            if not os.path.exists(sf) and not os.path.exists(lf):
                break
            docker_stats.extend(parse_docker_stats(sf))
            fps_values.extend(parse_device_log_fps(lf))
            latency_rows.extend(parse_latency_csv(laf))
            run_num += 1

    return {
        "agent": agent,
        "label": label,
        "docker_stats": docker_stats,
        "fps_values": fps_values,
        "latency_rows": latency_rows,
    }


def compute_summary(data):
    """Compute summary metrics from run data."""
    summary = {}
    ds = data["docker_stats"]
    if ds:
        cpus = [s["cpu_pct"] for s in ds]
        mems = [s["mem_mb"] for s in ds]
        summary["cpu_mean"] = statistics.mean(cpus)
        summary["cpu_median"] = statistics.median(cpus)
        summary["cpu_max"] = max(cpus)
        summary["mem_mean"] = statistics.mean(mems)
        summary["mem_max"] = max(mems)

    fps = data["fps_values"]
    if fps:
        summary["fps_steady"] = fps[-1]
        summary["fps_max"] = max(fps)

    lat = data["latency_rows"]
    if lat:
        e2e = [r["e2e_us"] for r in lat]
        summary["e2e_mean"] = statistics.mean(e2e)
        summary["e2e_p50"] = percentile(e2e, 50)
        summary["e2e_p90"] = percentile(e2e, 90)
        summary["e2e_p99"] = percentile(e2e, 99)
        summary["e2e_max"] = max(e2e)
        summary["frames"] = len(lat)
        if len(e2e) > 1:
            summary["jitter_cv"] = statistics.stdev(e2e) / statistics.mean(e2e)
            summary["tail_ratio"] = percentile(e2e, 99) / max(percentile(e2e, 50), 1)

    return summary


def print_single_report(data):
    """Print detailed report for one agent."""
    label = data["label"]
    ds = data["docker_stats"]
    fps = data["fps_values"]
    lat = data["latency_rows"]

    print(f"\n{'='*55}")
    print(f"  {label}")
    print(f"{'='*55}")

    if ds:
        cpus = [s["cpu_pct"] for s in ds]
        mems = [s["mem_mb"] for s in ds]
        print(f"  CPU %:     mean={statistics.mean(cpus):.1f}  "
              f"median={statistics.median(cpus):.1f}  "
              f"max={max(cpus):.1f}")
        print(f"  Memory:    mean={statistics.mean(mems):.1f} MB  "
              f"max={max(mems):.1f} MB")

    if fps:
        print(f"  FPS:       steady={fps[-1]:.1f}  max={max(fps):.1f}")

    if lat:
        print(f"  Frames:    {len(lat)}")

        e2e = [r["e2e_us"] for r in lat]
        infer = [r["inference_us"] for r in lat]
        preproc = [r["preprocess_us"] for r in lat]
        nms = [r["nms_us"] for r in lat]
        thumb = [r["thumbnail_us"] for r in lat]
        grpc = [r["grpc_us"] for r in lat]
        capture = [r.get("capture_us", 0) for r in lat]

        print(f"\n  Latency breakdown (us):")
        print(f"  {'Stage':<15} {'mean':>8} {'p50':>8} {'p90':>8} {'p99':>8} {'max':>8}")
        print(f"  {'-'*55}")
        for name, vals in [("capture", capture), ("preprocess", preproc),
                           ("inference", infer), ("nms", nms),
                           ("thumbnail", thumb), ("grpc_send", grpc),
                           ("E2E", e2e)]:
            if not vals or all(v == 0 for v in vals):
                continue
            print(f"  {name:<15} {statistics.mean(vals):>8.0f} "
                  f"{percentile(vals, 50):>8} "
                  f"{percentile(vals, 90):>8} "
                  f"{percentile(vals, 99):>8} "
                  f"{max(vals):>8}")

        rss = [r.get("rss_kb", 0) for r in lat]
        if any(v > 0 for v in rss):
            print(f"\n  RSS: mean={statistics.mean(rss):.0f} KB  "
                  f"max={max(rss)} KB")

        if len(e2e) > 1:
            cv = statistics.stdev(e2e) / statistics.mean(e2e)
            tail_ratio = percentile(e2e, 99) / max(percentile(e2e, 50), 1)
            print(f"  Jitter:    CV={cv:.3f}  p99/p50={tail_ratio:.2f}")


def print_comparison(data_a, data_b):
    """Print side-by-side comparison table for two agents."""
    sa = compute_summary(data_a)
    sb = compute_summary(data_b)
    la = data_a["label"]
    lb = data_b["label"]

    print(f"\n{'='*60}")
    print(f"  Comparison: {la} vs {lb}")
    print(f"{'='*60}")
    print(f"  {'Metric':<25} {la:>15} {lb:>15}")
    print(f"  {'-'*55}")

    rows = [
        ("Frames", "frames", "{:.0f}", ""),
        ("CPU % (mean)", "cpu_mean", "{:.1f}", "%"),
        ("Memory (mean)", "mem_mean", "{:.1f}", " MB"),
        ("Memory (max)", "mem_max", "{:.1f}", " MB"),
        ("FPS (steady)", "fps_steady", "{:.1f}", ""),
        ("E2E latency mean", "e2e_mean", "{:.0f}", " us"),
        ("E2E latency p50", "e2e_p50", "{:.0f}", " us"),
        ("E2E latency p90", "e2e_p90", "{:.0f}", " us"),
        ("E2E latency p99", "e2e_p99", "{:.0f}", " us"),
        ("Jitter CV", "jitter_cv", "{:.3f}", ""),
        ("Tail ratio p99/p50", "tail_ratio", "{:.2f}", "x"),
    ]

    for label, key, fmt, unit in rows:
        va = sa.get(key)
        vb = sb.get(key)
        va_str = (fmt.format(va) + unit) if va is not None else "N/A"
        vb_str = (fmt.format(vb) + unit) if vb is not None else "N/A"
        print(f"  {label:<25} {va_str:>15} {vb_str:>15}")

    # Speedup summary
    if "e2e_mean" in sa and "e2e_mean" in sb:
        faster = sa["e2e_mean"] / sb["e2e_mean"] if sb["e2e_mean"] > 0 else 0
        if faster < 1:
            print(f"\n  >> {la} is {1/faster:.2f}x faster (E2E mean)")
        elif faster > 1:
            print(f"\n  >> {lb} is {faster:.2f}x faster (E2E mean)")
        else:
            print(f"\n  >> Both agents have similar E2E latency")


def main():
    if len(sys.argv) < 2:
        print(f"Usage:")
        print(f"  {sys.argv[0]} <run_dir>                   # single agent report")
        print(f"  {sys.argv[0]} <run_dir_a> <run_dir_b>     # comparison report")
        sys.exit(1)

    dirs = [d for d in sys.argv[1:] if os.path.isdir(d)]
    if not dirs:
        print(f"ERROR: No valid directories found in arguments")
        sys.exit(1)

    if len(dirs) == 1:
        # Single directory mode
        run_dir = dirs[0]
        print(f"Analyzing: {run_dir}")
        data = load_run_data(run_dir)
        if not data["docker_stats"] and not data["fps_values"] and not data["latency_rows"]:
            print(f"No data found in {run_dir}")
            sys.exit(1)
        print_single_report(data)
    else:
        # Dual directory comparison mode
        print(f"Analyzing:")
        print(f"  A: {dirs[0]}")
        print(f"  B: {dirs[1]}")

        data_a = load_run_data(dirs[0])
        data_b = load_run_data(dirs[1])

        print_single_report(data_a)
        print_single_report(data_b)
        print_comparison(data_a, data_b)


if __name__ == "__main__":
    main()
