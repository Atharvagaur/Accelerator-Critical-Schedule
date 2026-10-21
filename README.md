# Accelerator-Critical CPU Scheduling with `sched_ext`

An experimental Linux CPU scheduler built with **`sched_ext` and eBPF** that explores prioritizing accelerator-critical tasks to reduce GPU idle time under CPU contention.

The goal is to identify CPU threads that may be delaying GPU work and give them bounded scheduling priority, without requiring modifications to the machine-learning applications themselves.

> **Status:** Experimental. Benchmark results are preliminary, and scheduler stability under extreme contention remains under investigation.

## Overview

Modern ML workloads rely on CPU threads to prepare data, launch GPU kernels, and coordinate execution. Under heavy CPU contention, delays in these threads can leave the GPU waiting for work.

This project explores an accelerator-aware scheduling strategy that combines GPU idle-time tracking, contention-aware task criticality, and starvation prevention.

## Key Features

### 1. Temporal GPU Idle-Fraction Tracking

A user-space feeder tracks GPU activity transitions and calculates idle time over one-second measurement windows.

* Measures accumulated idle duration in nanoseconds.
* Publishes the idle fraction to a BPF map using a fixed-point representation from `0` to `1000`.
* Uses `1000` to represent fully idle and `0` to represent fully active during the measurement window.
* Handles measurement windows that cross multiple processing intervals.

**Validation:** A synthetic workload with equal active and idle durations produced an idle-fraction value of `500`.

### 2. Contention-Aware Criticality Inference

The scheduler estimates task criticality using scheduling delays observed while the GPU is idle.

* **Attribution:** Tasks experiencing scheduling delay during GPU idle periods receive criticality credit.
* **Contention scaling:** Priority bonuses increase with runnable-task count, up to a configured threshold.
* **Bounded priority:** Criticality bonuses are capped to limit interference with ordinary scheduling.
* **Adaptive behavior:** The design aims to preserve fair scheduling under low contention while prioritizing potentially accelerator-critical tasks under heavier contention.

The bonus follows:

$$
B = B_{\max}\cdot
\frac{\min(N_r,N_{\text{th}})}{N_{\text{th}}}
$$

Where:

* \(B\) is the applied priority bonus.
* \(B_{\max}\) is the maximum bonus.
* \(N_r\) is the runnable-task count.
* \(N_{\text{th}}\) is the contention threshold.

### 3. Starvation Prevention

A starvation guard detects tasks that have waited longer than a configured limit.

* **Wait threshold:** 500 ms.
* **Recovery action:** Sets the task's virtual time (`vtime`) to zero to encourage earlier dispatch.
* **Objective:** Reduce the risk of indefinite delays for non-critical tasks.

This mechanism is a mitigation rather than a formal guarantee of starvation freedom.

### 4. Performance Metrics and EMA

The project records metrics to evaluate scheduling behavior and performance.

* GPU idle fraction.
* Step time.
* Iteration latency.
* Runnable-task count.
* Exponential moving averages (EMA) for smoothing selected performance metrics.

EMA helps reduce short-term noise in reported measurements. Raw measurements should still be retained for percentile analysis and reproducibility.

## Preliminary Evaluation

The current evaluation uses a 119-sample dataset to compare scheduler behavior against a baseline.

| Metric            |              Mean delta |               p50 delta |              p95 delta |
| ----------------- | ----------------------: | ----------------------: | ---------------------: |
| GPU idle          | −1.40 percentage points | −2.30 percentage points | 0.00 percentage points |
| Step time         |               −43.10 ms |                +7.86 ms |               −9.20 ms |
| Iteration latency |                +2.28 ms |                −0.21 ms |               +2.71 ms |
| Runnable tasks    |                   +25.5 |                      +9 |                     +6 |

Deltas are calculated as scheduler measurements minus baseline measurements. Negative deltas indicate lower values; whether that is beneficial depends on the metric.

> **Metric fix (critical):** The `step_time` metric was redefined and the measurement bug fixed in `src/accel_critical.bpf.c`. Previously `step_time` was measured as the wall-clock interval between consecutive dispatches of a task (previous CPU burst + off-CPU wait + queueing delay), which is *not* the CPU-side portion of an iteration and legitimately exceeded iteration latency. It is now measured as the actual CPU execution time per iteration (`running()` start → `stopping()`). All step-time figures reported above (and in any Table II/V derived from the old CSVs) were produced with the buggy definition and are **invalidated**. Regenerate `metrics.csv`/`baseline_metrics.csv` with the fixed scheduler (`sudo ./accel_critical --csv`) and re-run `results.py` before citing any step-time improvement as evidence of scheduler performance.

### Initial Observations

* Median GPU idle time decreased by 2.30 percentage points.
* Mean step time: **figure invalidated by the step-time redefinition** — must be regenerated with the fixed measurement (see note above).
* Mean and p95 iteration latency increased slightly.
* Runnable-task counts increased relative to the baseline.
* The correlation between runnable-task count and GPU idle fraction was approximately −0.013, indicating virtually no linear relationship in this dataset.

**Interpretation:** Results are mixed. Some metrics improved, while others regressed. Repeated, controlled experiments are required before drawing conclusions about overall performance.

## Stability Analysis: The Contention Paradox

Stress testing with `stress-ng` exposed a potential scheduler-liveness problem under extreme oversubscription.

### Observed behavior

The kernel watchdog occasionally ejected the scheduler after reporting prolonged runnable-task stalls, with waits exceeding 40 seconds.

### Initial analysis

Some tasks in specific dispatch queues (DSQs) appeared not to be dispatched despite having favorable virtual-time values.

This suggests that virtual-time preference alone may not be sufficient to ensure timely progress under extreme contention.

### Future investigation

* Inspect dispatch-queue selection and task-enqueue paths.
* Verify that eligible tasks are consistently dispatched.
* Investigate whether scheduler kicks or more aggressive preemption are needed.
* Evaluate starvation behavior across different contention levels.
* Repeat stress tests while collecting scheduler traces and watchdog diagnostics.

The precise cause of these stalls has not yet been established.

## Architecture

The project consists of three conceptual components:

1. **GPU activity monitoring:** Measures GPU idle time and publishes the result.
2. **Criticality inference:** Uses observed scheduling delays and GPU idle state to estimate task importance.
3. **Scheduling policy:** Applies contention-scaled priority bonuses and starvation mitigation.

```text
GPU Activity
     |
     v
Idle-Time Measurement
     |
     v
BPF Metrics Map
     |
     v
Criticality Inference
     |
     v
Contention-Scaled Priority
     |
     v
sched_ext Dispatch
     |
     v
CPU Task Execution
```

## Tech Stack

* **Kernel scheduling:** Linux `sched_ext`
* **Kernel-side implementation:** eBPF C
* **User-space implementation:** C
* **Metrics:** CSV, BPF maps
* **Testing:** `stress-ng`
* **Inspection and diagnostics:** `bpftool`, `libbpf`

## Getting Started

### Prerequisites

* Linux kernel with support for `sched_ext`.
* A compatible `scx` source tree and build environment.
* Clang/LLVM, libbpf, and other dependencies required by the project.
* Root privileges to load and run the scheduler.
* An available GPU telemetry source supported by the monitoring implementation.

### Build

From the project root:

```bash
make SCX_DIR=/home/vpekka/scx
```

Adjust `SCX_DIR` to point to your local `scx` source tree.

### Run

```bash
sudo ./accel_critical --csv
```

This starts the scheduler and enables CSV metric collection, assuming the binary and its required runtime dependencies are available.

### Test Under CPU Contention

In a separate terminal, run:

```bash
stress-ng --cpu 4
```

Adjust the worker count to explore different levels of CPU contention. Use a controlled workload and a consistent benchmark duration when comparing scheduler configurations.

**Safety note:** Experimental schedulers can affect system responsiveness. Test on a recoverable system, retain a way to stop the workload, and understand the scheduler's fallback behavior before running prolonged stress tests.

### Analyze Metrics

The CSV output is intended to contain fields such as:

```text
timestamp,gpu_idle_pct,step_time_us,iteration_latency_us,runnable_count
```

Verify the actual header emitted by your implementation before using this example.

Use Python, pandas, or a spreadsheet to compare baseline and scheduler measurements. Examine mean, median, p95, maximum values, and variability across repeated runs.

## Limitations

* The current benchmark results are preliminary.
* Scheduling delays during GPU idle periods do not, by themselves, prove that a task is on the GPU's critical path.
* Correlation does not establish causation.
* The scheduler has exhibited liveness issues under extreme contention.
* Performance can vary with workload, hardware, kernel configuration, and measurement methodology.
* A reduction in GPU idle time does not necessarily imply improved end-to-end application performance.

## Roadmap

* [ ] Diagnose and resolve prolonged runnable-task stalls.
* [ ] Improve dispatch liveness under extreme contention.
* [ ] Compare against a baseline using repeated, controlled workloads.
* [ ] Report confidence intervals and run-to-run variability.
* [ ] Evaluate p50 and p95 application latency alongside GPU idle time.
* [ ] Test across different contention levels and workload types.
* [ ] Document supported kernel versions, hardware, and dependencies.

## Project Goal

Explore whether accelerator-critical CPU scheduling can improve GPU feeding behavior under contention while preserving fairness and scheduler stability.

The central research question is:

**Can runtime GPU-idleness signals help a CPU scheduler identify and prioritize tasks that matter most to accelerator progress, without sacrificing system-wide fairness or liveness?**
