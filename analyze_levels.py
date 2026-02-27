#!/usr/bin/env python3
"""Analyze pipeline level 0-4 benchmark CSVs side by side.

Key insight: E2E latency ≠ throughput for pipelined systems.
- L0 (single-thread): E2E = sum of stages = inter-frame time, so FPS = 1/E2E
- L1-L4 (pipelined): E2E includes queue wait time. Throughput is limited by
  the slowest stage (inference), not by E2E.
  Real throughput = total_frames / wall_clock_time
"""
import csv, statistics, os, sys

def load_csv(path):
    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append({k: int(v) for k, v in r.items()})
    return rows

def p(data, pct):
    s = sorted(data)
    idx = min(int(len(s) * pct / 100), len(s) - 1)
    return s[idx]

def analyze(rows):
    e2e = [r["e2e_us"] for r in rows]
    cap = [r["capture_us"] for r in rows]
    pre = [r["preprocess_us"] for r in rows]
    inf = [r["inference_us"] for r in rows]
    nms = [r["nms_us"] for r in rows]
    thu = [r["thumbnail_us"] for r in rows]
    grp = [r["grpc_us"] for r in rows]

    # Pure stage cost (excluding queue wait) = sum of measured stages
    # For L0: this ≈ e2e (no queue wait)
    # For L1-L4: e2e - stage_sum = queue wait time
    stage_sums = [c + p_ + i + n + t + g for c, p_, i, n, t, g
                  in zip(cap, pre, inf, nms, thu, grp)]
    queue_waits = [e - s for e, s in zip(e2e, stage_sums)]

    return {
        "frames": len(rows),
        "e2e_mean": statistics.mean(e2e),
        "e2e_p50": p(e2e, 50),
        "e2e_p90": p(e2e, 90),
        "e2e_p99": p(e2e, 99),
        "cap_mean": statistics.mean(cap),
        "pre_mean": statistics.mean(pre),
        "inf_mean": statistics.mean(inf),
        "nms_mean": statistics.mean(nms),
        "thu_mean": statistics.mean(thu),
        "grp_mean": statistics.mean(grp),
        "stage_sum_mean": statistics.mean(stage_sums),
        "queue_wait_mean": statistics.mean(queue_waits),
        "queue_wait_p50": p(queue_waits, 50),
        "jitter_cv": statistics.stdev(e2e) / statistics.mean(e2e) if len(e2e) > 1 else 0,
    }

# Load all levels
levels = [0, 1, 2, 3, 4]
results = {}
raw = {}
for lv in levels:
    path = f"data/bench_l{lv}.csv"
    if os.path.exists(path):
        raw[lv] = load_csv(path)
        results[lv] = analyze(raw[lv])
        print(f"L{lv}: {path} ({results[lv]['frames']} frames)")
    else:
        print(f"L{lv}: {path} NOT FOUND")

W = 12  # column width

def row(label, vals):
    print(f"{label:<24}", end="")
    for v in vals:
        print(f"{v:>{W}}", end="")
    print()

def sep():
    print("-" * (24 + W * 5))

# ========== Table 1: Per-frame latency ==========
print()
print("=" * (24 + W * 5))
print("  TABLE 1: Per-Frame Latency (us)")
print("=" * (24 + W * 5))
row("", [f"L{lv}" for lv in levels])
sep()

metrics = [
    ("Frames",           "frames",          "{:.0f}"),
    ("E2E mean",         "e2e_mean",        "{:.0f}"),
    ("E2E p50",          "e2e_p50",         "{:.0f}"),
    ("E2E p90",          "e2e_p90",         "{:.0f}"),
    ("E2E p99",          "e2e_p99",         "{:.0f}"),
    ("Jitter CV",        "jitter_cv",       "{:.3f}"),
]
for label, key, fmt in metrics:
    vals = []
    for lv in levels:
        if lv in results:
            vals.append(fmt.format(results[lv][key]))
        else:
            vals.append("N/A")
    row(label, vals)

# ========== Table 2: Stage breakdown ==========
print()
print("=" * (24 + W * 5))
print("  TABLE 2: Stage Breakdown — mean (us)")
print("  L1-L4: preprocess includes acquire_write_slot() wait")
print("=" * (24 + W * 5))
row("", [f"L{lv}" for lv in levels])
sep()

stages = [
    ("Capture",      "cap_mean"),
    ("Preprocess",   "pre_mean"),
    ("Inference",    "inf_mean"),
    ("NMS",          "nms_mean"),
    ("Thumbnail",    "thu_mean"),
    ("gRPC send",    "grp_mean"),
    ("Stage sum",    "stage_sum_mean"),
    ("Queue wait *", "queue_wait_mean"),
    ("E2E total",    "e2e_mean"),
]
for label, key in stages:
    vals = []
    for lv in levels:
        if lv in results:
            vals.append(f"{results[lv][key]:.0f}")
        else:
            vals.append("N/A")
    row(label, vals)

print()
print("  * Queue wait = E2E - stage_sum")
print("    L0: ~0 (no queue, sequential)")
print("    L1: unbounded queue → frames pile up → wait grows over time")
print("    L2: bounded queue → backpressure, moderate wait")
print("    L3/L4: ring buffer → backpressure, similar wait")

# ========== Table 3: Throughput ==========
print()
print("=" * (24 + W * 5))
print("  TABLE 3: Throughput")
print("=" * (24 + W * 5))
row("", [f"L{lv}" for lv in levels])
sep()

# For L0: throughput = 1/e2e_mean (sequential)
# For L1-L4: throughput ≈ 1/bottleneck = 1/inference_mean (pipelined)
# Real throughput from data: we can estimate from frame timestamps
throughput_vals = []
for lv in levels:
    if lv not in results:
        throughput_vals.append("N/A")
        continue
    r = results[lv]
    if lv == 0:
        # Sequential: FPS = 1/e2e
        fps = 1_000_000 / r["e2e_mean"]
    else:
        # Pipelined: FPS ≈ 1/bottleneck_stage (inference)
        # More accurate: total_frames / total_wall_time
        # Approximate wall time from first frame's e2e start to last frame's e2e end
        # But we don't have absolute timestamps, so use inference as bottleneck
        fps = 1_000_000 / r["inf_mean"]
    throughput_vals.append(f"{fps:.1f}")

row("Throughput FPS", throughput_vals)

# Bottleneck stage
for lv in levels:
    if lv not in results:
        continue
    r = results[lv]
    if lv == 0:
        print(f"  L{lv}: sequential → bottleneck = entire pipeline ({r['e2e_mean']/1000:.0f} ms/frame)")
    else:
        print(f"  L{lv}: pipelined → bottleneck = inference ({r['inf_mean']/1000:.0f} ms/frame)")

# ========== L1 queue growth analysis ==========
if 1 in raw:
    print()
    print("=" * 60)
    print("  L1 Queue Growth Analysis (unbounded queue problem)")
    print("=" * 60)
    data = raw[1]
    n = len(data)
    chunks = 5
    chunk_size = n // chunks
    for i in range(chunks):
        start = i * chunk_size
        end = start + chunk_size if i < chunks - 1 else n
        chunk = data[start:end]
        e2e_chunk = [r["e2e_us"] for r in chunk]
        mean_ms = statistics.mean(e2e_chunk) / 1000
        print(f"  Frames {chunk[0]['frame']:>5}-{chunk[-1]['frame']:>5}: "
              f"E2E mean = {mean_ms:>10.0f} ms  "
              f"({mean_ms/1000:.1f} sec)")
    print()
    print("  → E2E grows linearly because recv is faster than infer.")
    print("    Each frame waits longer in the unbounded queue.")
    print("    This is why bounded queue (L2+) is necessary.")
