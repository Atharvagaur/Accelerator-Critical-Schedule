#!/usr/bin/env python3
"""Summarize accel_critical metrics CSVs and compare a run against a baseline.

Usage:
    python3 results.py [--warmup N] [--min-samples N] [metrics.csv] [baseline.csv]

Inputs must come from `sudo ./accel_critical --csv` with the header:
    timestamp,gpu_idle_fraction,cpu_slice_us,iter_latency_us,runnable_tasks

Notes on method:
  * The first N samples (default 2) are dropped as warm-up. The count is printed.
  * Percentiles use the nearest-rank method for every statistic.
  * Scheduler and baseline are independent runs, so the baseline comparison uses
    independent-sample differences of mean/p50/p95. Samples are not paired.
  * gpu_idle_fraction is a 0..1000 fixed-point value; it is printed as a percentage.
  * The GPU signal produced by the loader is synthetic (see README). Results from it
    show scheduler behavior under a simulated accelerator, not measured GPU idle time.
  * Correlations between time-series samples are descriptive only, not causal evidence.
"""

import argparse
import csv
import math
import statistics
import sys

REQUIRED_COLUMNS = [
    "timestamp",
    "gpu_idle_fraction",
    "cpu_slice_us",
    "iter_latency_us",
    "runnable_tasks",
]

METRICS = {
    "gpu_idle_fraction": "GPU Idle (%)",
    "cpu_slice_us": "CPU Slice (us)",
    "iter_latency_us": "Iteration Latency (us)",
    "runnable_tasks": "Runnable Tasks",
}

MIN_SAMPLES_DEFAULT = 30
WARMUP_DEFAULT = 2


class InputError(Exception):
    pass


def load_csv(path, warmup):
    """Return (columns, warmup_dropped) with numeric values, or raise InputError."""
    try:
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            header = reader.fieldnames or []
            missing = [c for c in REQUIRED_COLUMNS if c not in header]
            if missing:
                raise InputError(
                    f"{path}: missing required column(s) {missing}; "
                    f"found {header}. Re-collect with the current loader."
                )
            rows = list(reader)
    except FileNotFoundError:
        raise InputError(f"{path}: file not found")

    if len(rows) <= warmup:
        raise InputError(f"{path}: {len(rows)} data row(s), not more than warm-up of {warmup}")

    columns = {c: [] for c in REQUIRED_COLUMNS}
    for lineno, row in enumerate(rows[warmup:], start=warmup + 2):
        for c in REQUIRED_COLUMNS:
            raw = (row.get(c) or "").strip()
            try:
                columns[c].append(float(raw))
            except ValueError:
                raise InputError(f"{path}:{lineno}: non-numeric {c}={raw!r}")

    ts = columns["timestamp"]
    if any(b <= a for a, b in zip(ts, ts[1:])):
        raise InputError(f"{path}: timestamps are not strictly increasing")

    return columns, warmup


def nearest_rank(sorted_vals, p):
    """Nearest-rank percentile for p in (0, 1]."""
    idx = max(0, math.ceil(p * len(sorted_vals)) - 1)
    return sorted_vals[idx]


def summarize(values):
    s = sorted(values)
    return {
        "n": len(s),
        "mean": statistics.mean(s),
        "p50": nearest_rank(s, 0.50),
        "p95": nearest_rank(s, 0.95),
        "max": s[-1],
    }


def scale(col, value):
    """Gpu idle is stored as 0..1000; show it as a percentage."""
    return value / 10.0 if col == "gpu_idle_fraction" else value


def fmt(col, value):
    if col == "gpu_idle_fraction":
        return f"{value:>9.2f}%"
    return f"{value:>10.1f}"


def pearson(x, y):
    if len(x) != len(y) or len(x) < 2:
        return None
    mx, my = statistics.mean(x), statistics.mean(y)
    num = sum((a - mx) * (b - my) for a, b in zip(x, y))
    den = math.sqrt(sum((a - mx) ** 2 for a in x) * sum((b - my) ** 2 for b in y))
    return num / den if den else 0.0


def print_summary(path, cols, warmup):
    print("=" * 84)
    print(f" RESULTS: {path}")
    print("=" * 84)
    print(f"Warm-up samples excluded: {warmup}")
    print(f"Samples analyzed: {len(cols['timestamp'])}")
    print("-" * 84)
    print(f"{'Metric':<25} | {'n':>5} | {'Mean':>10} | {'p50':>10} | {'p95':>10} | {'Max':>10}")
    print("-" * 84)
    for col, label in METRICS.items():
        s = summarize(cols[col])
        print(
            f"{label:<25} | {s['n']:>5} | {fmt(col, scale(col, s['mean']))} | "
            f"{fmt(col, scale(col, s['p50']))} | {fmt(col, scale(col, s['p95']))} | "
            f"{fmt(col, scale(col, s['max']))}"
        )
    print("=" * 84)


def print_baseline(main_cols, base_cols, base_path, min_samples):
    print("\nBaseline comparison (independent runs; deltas = scheduler - baseline)")
    print(f"Baseline: {base_path}")
    n_main = len(main_cols["timestamp"])
    n_base = len(base_cols["timestamp"])
    print(f"Samples: scheduler={n_main}, baseline={n_base}")
    if min(n_main, n_base) < min_samples:
        print(f"WARNING: fewer than {min_samples} samples in at least one run; "
              "differences are not reliable evidence.")
    print("-" * 84)
    for col, label in METRICS.items():
        m = summarize(main_cols[col])
        b = summarize(base_cols[col])
        print(
            f"{label:<25} | Mean Δ: {fmt(col, scale(col, m['mean'] - b['mean']))} | "
            f"p50 Δ: {fmt(col, scale(col, m['p50'] - b['p50']))} | "
            f"p95 Δ: {fmt(col, scale(col, m['p95'] - b['p95']))}"
        )
    print("-" * 84)


def print_correlations(cols):
    print("\nDescriptive correlations (time-series samples; not causal evidence)")
    r = pearson(cols["runnable_tasks"], cols["gpu_idle_fraction"])
    if r is None:
        print("Runnable vs GPU idle: N/A (need at least 2 samples)")
    else:
        print(f"Runnable vs GPU idle: {r:+.4f} ({'weak or none' if abs(r) < 0.5 else 'strong'})")
    r = pearson(cols["cpu_slice_us"], cols["gpu_idle_fraction"])
    if r is None:
        print("CPU slice vs GPU idle: N/A (need at least 2 samples)")
    else:
        print(f"CPU slice vs GPU idle: {r:+.4f} ({'weak or none' if abs(r) < 0.5 else 'strong'})")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("main_csv", nargs="?", default="metrics.csv")
    parser.add_argument("baseline_csv", nargs="?", default=None)
    parser.add_argument("--warmup", type=int, default=WARMUP_DEFAULT,
                        help=f"samples to drop from the start of each file (default {WARMUP_DEFAULT})")
    parser.add_argument("--min-samples", type=int, default=MIN_SAMPLES_DEFAULT,
                        help=f"warn below this many samples per run (default {MIN_SAMPLES_DEFAULT})")
    args = parser.parse_args(argv)

    if args.warmup < 0:
        parser.error("--warmup must be >= 0")

    try:
        main_cols, warmup = load_csv(args.main_csv, args.warmup)
        base_cols = load_csv(args.baseline_csv, args.warmup)[0] if args.baseline_csv else None
    except InputError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    print_summary(args.main_csv, main_cols, warmup)
    if base_cols is not None:
        print_baseline(main_cols, base_cols, args.baseline_csv, args.min_samples)
    print_correlations(main_cols)
    return 0


if __name__ == "__main__":
    sys.exit(main())
