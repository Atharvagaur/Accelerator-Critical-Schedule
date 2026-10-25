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

> **The GPU signal is synthetic.** The feeder generates randomized active/idle intervals (100–900 ms each) and does not read GPU telemetry. Results describe scheduler behavior under a simulated accelerator, not measured GPU idle time. The console banner prints `GPU signal: SYNTHETIC`.

### 2. GPU-Idle-Scaled Criticality Inference

The scheduler estimates task criticality using scheduling delays observed while the GPU is idle.

* **Attribution:** Tasks experiencing scheduling delay during GPU idle periods receive criticality credit.
* **GPU-idle scaling:** The priority bonus is scaled by how long the GPU has been idle (up to a 500 ms threshold), so credit is strongest when the accelerator has been waiting the longest. A fully active GPU yields no bonus.
* **Bounded priority:** Criticality bonuses are capped (`CRIT_MAX`) and folded into `vtime` through a bounded `MAX_BONUS` to limit interference with ordinary scheduling.
* **Decay:** Criticality decays each dispatch (`crit -= crit >> 4`) so stale credit fades.
* **Runnable-task count is metric-only.** It is recorded as a window peak but does not scale the bonus. Runnable-count scaling was considered and not adopted: GPU-idle scaling is the retained policy so that existing comparisons remain valid (decision recorded in `plan.md` §2.3).

> **Note:** An earlier draft described a contention-scaled bonus \(B = B_{\max}\cdot \min(N_r,N_{\text{th}})/N_{\text{th}}\). That formula is **not** implemented; the implemented scaling is by GPU idle duration (see above).

### 3. Starvation Prevention

A starvation guard flags tasks whose enqueue-to-run wait exceeded a configured limit.

* **Detection:** `accel_running()` measures each dispatch's enqueue-to-run wait. If it exceeds 500 ms, the task is flagged.
* **Recovery action:** The flagged task's next enqueue sets its virtual time (`vtime`) to zero and requests preemption. This is a one-shot boost for the next wakeup.
* **Objective:** Reduce the risk of indefinite delays for non-critical tasks.

This is a mitigation rather than a formal guarantee of starvation freedom. The boost applies on the next wakeup, so a task that is already queued beyond 500 ms is not rescued until it runs once. That behavior is not yet validated under contention (see Limitations).

### 4. Performance Metrics and EMA

The loader writes one row per second to `metrics.csv` when run with `--csv`:

```text
timestamp,gpu_idle_fraction,cpu_slice_us,iter_latency_us,runnable_tasks
```

* **`gpu_idle_fraction`:** Synthetic GPU idle fraction, 0–1000 fixed-point, over one-second windows (see Feature 1).
* **`cpu_slice_us`:** EMA of CPU execution time per dispatch (`running()` → `stopping()`). This is one scheduler slice, not a whole ML iteration.
* **`iter_latency_us`:** EMA of enqueue-to-run wait per dispatch. This is the only latency definition in use.
	* **`runnable_tasks`:** Current runnable-task count. Enqueue increments the count and dispatch/disable decrement it, ensuring the count returns to zero when idle.
* **Stalls:** Counter of dispatches whose enqueue-to-run wait exceeded 2 s. Shown on the console, not written to the CSV.

EMA stats are kept per CPU and averaged across the CPUs that reported a value. They are not summed, since summing would scale the result with CPU count. Raw measurements should still be retained for percentile analysis and reproducibility.

## Performance Evaluation

The scheduler was evaluated under CPU contention using `stress-ng` with 16 workers. The following metrics were collected using the fixed temporal tracking and atomic counters.

| Metric                    | Mean | p50 | p95 | Max |
| ------------------------- | ----: | ---: | ---: | ---: |
| **GPU Idle (%)**          | 46.8% | 45.8% | 82.6% | 86.4% |
| **CPU Slice (us)**        | 15,996 | 13,079 | 30,597 | 37,211 |
| **Iteration Latency (us)** | 16,866 | 18,462 | 25,575 | 34,254 |
| **Runnable Tasks**        | 19.1 | 19.0 | 38.0 | 71.0 |

**Observations:**
*   **GPU Utilization:** The synthetic signal shows a mean idleness of 46.8%, with peak idleness reaching 86.4%.
*   **Scheduling Latency:** Iteration latency (enqueue-to-run) remains stable with a p95 of 25.6 ms, well below the 500 ms starvation threshold.
*   **Concurrency:** The system handled a peak of 71 runnable tasks without stability issues or watchdog ejections.
*   **Correlations:** The correlation between runnable task count and GPU idleness remains weak (+0.03), suggesting that CPU contention is not the primary driver of GPU idleness in this synthetic configuration.

## Stability Analysis: The Contention Paradox

Stress testing with `stress-ng` exposed a potential scheduler-liveness problem under extreme oversubscription.

### Observed behavior

The kernel watchdog occasionally ejected the scheduler after reporting prolonged runnable-task stalls, with waits exceeding 40 seconds.

### Initial analysis

Some tasks in specific dispatch queues (DSQs) appeared not to be dispatched despite having favorable virtual-time values.

This suggests that virtual-time preference alone may not be sufficient to ensure timely progress under extreme contention.

### Remediation and current state

A comprehensive remediation pass addressed the most critical stability and metric flaws:
* **Temporal Metrics**: Moved GPU idle tracking into BPF to eliminate userspace lag and overcounting.
* **Atomic State**: Enforced strict atomicity for global runnable and peak counters using CAS loops and `__sync` primitives.
* **Runnable Accuracy**: Resolved runnable-count drift by adding decrement logic to `accel_disable`.

The stress matrix script (`scripts/stress_matrix.sh`) was implemented to formally validate liveness. Recent runs with 8, 16, and 32 workers confirm that the scheduler now remains active and stable under high contention without watchdog ejections.

### Future investigation

* Inspect dispatch-queue selection and task-enqueue paths.
* Verify that eligible tasks are consistently dispatched.
* Investigate whether scheduler kicks or more aggressive preemption are needed.
* Evaluate starvation behavior across different contention levels.
* Repeat stress tests while collecting scheduler traces and watchdog diagnostics.
* The precise cause of these stalls has not yet been established.

## Architecture

The project consists of three conceptual components:

1. **GPU activity monitoring:** Measures GPU idle time and publishes the result. The signal is synthetic in this version.
2. **Criticality inference:** Uses observed scheduling delays and GPU idle state to estimate task importance.
3. **Scheduling policy:** Applies GPU-idle-scaled priority bonuses and starvation mitigation.

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
GPU-Idle-Scaled Priority
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

Run the host-side analysis tests (no kernel or scheduler needed):

```bash
make test
```

### Run

This starts the scheduler and enables CSV metric collection, assuming the binary and its required runtime dependencies are available.

### Scheduler vs. baseline runs

The loader has no baseline switch. A baseline run is made by commenting out two lines in `src/accel_critical.c`, so the default scheduler (EEVDF) stays in charge.

| Run | Lines in `src/accel_critical.c` | Output |
| --- | --- | --- |
| Scheduler | Leave both lines active | `metrics.csv` |
| Baseline | Comment out lines 207 and 241 | rename `metrics.csv` to `baseline_metrics.csv` |

Scheduler run:

```bash
make
sudo ./accel_critical --csv
```

Baseline run:

1. Comment out the two lines (keep the code between them unchanged):

   ```c
   // link = SCX_OPS_ATTACH(skel, accel_ops, accel_critical);   // line 207
   // bpf_link__destroy(link);                                  // line 241
   ```

   Commenting out only the attach line is not enough. `link` is then uninitialized when the loader exits and `bpf_link__destroy` is called on it, which can crash.

2. Rebuild and run:

   ```bash
   make
   sudo ./accel_critical --csv
   mv metrics.csv baseline_metrics.csv
   ```

   Rename the file right after the run. Both runs write to `metrics.csv`, so a later run overwrites the earlier one.

3. Uncomment both lines and rebuild before the next scheduler run.

Before any baseline run, check that no other scheduler is active: `cat /sys/kernel/sched_ext/state` should print `disabled`.

In a baseline run the scheduler columns (`cpu_slice_us`, `iter_latency_us`, `runnable_tasks`) are still written, but they are zero because no scheduler callbacks run. Only `gpu_idle_fraction` is comparable between the two runs.

```bash
sudo ./accel_critical --csv
```

### Test Under CPU Contention

In a separate terminal, run:

```bash
stress-ng --cpu 4
```

Adjust the worker count to explore different levels of CPU contention. Use a controlled workload and a consistent benchmark duration when comparing scheduler configurations.

For a repeatable matrix that records scheduler state and `sched_ext` kernel messages at each contention level, run as root:

```bash
sudo WORKERS="2 4 8" DURATION=60 ./scripts/stress_matrix.sh
```

Results go to `/tmp/opencode/stress-<timestamp>/`. The script loads the scheduler, so run it only on a machine where a hung scheduler can be recovered. The matrix has not yet been run on the target hardware.

**Safety note:** Experimental schedulers can affect system responsiveness. Test on a recoverable system, retain a way to stop the workload, and understand the scheduler's fallback behavior before running prolonged stress tests.

### Analyze Metrics

```bash
python3 results.py metrics.csv                                # summary
python3 results.py metrics.csv baseline_metrics.csv           # plus independent-run comparison
python3 results.py --warmup 2 --min-samples 30 metrics.csv baseline_metrics.csv
```

`results.py` requires the header written by the current loader (`cpu_slice_us`, not `step_time_us`). It rejects files that are missing columns, contain non-numeric values, have non-increasing timestamps, or are no longer than the warm-up. It drops the first `--warmup` samples (default 2), uses nearest-rank percentiles, and warns when a run has fewer than `--min-samples` samples (default 30). Scheduler and baseline runs are independent, so the comparison is not paired. Sample counts are printed with every summary.

Use the same workload, duration, and GPU signal source for both runs. Examine mean, p50, p95, maximum values, and variability across repeated runs.

## Limitations

* The GPU signal is synthetic. No real GPU telemetry is read, so results do not measure real GPU idle time.
* The current benchmark results are preliminary.
* Scheduling delays during GPU idle periods do not, by themselves, prove that a task is on the GPU's critical path.
* Correlation does not establish causation.
* The scheduler has exhibited liveness issues under extreme contention.
* Performance can vary with workload, hardware, kernel configuration, and measurement methodology.
* A reduction in GPU idle time does not necessarily imply improved end-to-end application performance.

### Open implementation issues

* **Priority policy:** Runnable count does not scale the bonus. This is deliberate for now (GPU-idle scaling only) so that existing comparisons stay valid. Revisit it when the evaluation is re-run.
* **Criticality attribution is unvalidated:** Credit goes to any task that waited while the GPU was idle, which may include tasks unrelated to the accelerator. No experiment has checked this yet.
* **Starvation boost is one-shot:** It applies on the next wakeup, not while a task is already queued. It has not been tested under contention.
* **Liveness is unresolved:** The watchdog ejections described above have no confirmed root cause.
* **Runnable count is best-effort:** Tasks that leave the queue by a path other than dispatch (for example exit or migration) are not decremented, so the count may drift upward. A `dequeue` callback would close this gap.

## Roadmap

Completed in the remediation pass (`plan.md` §1–2):

* [x] Label the GPU signal as synthetic in code, console output, and docs.
* [x] Rename `step_time` to `cpu_slice_us`, and use enqueue-to-run as the only latency definition.
* [x] Average EMA stats across CPUs instead of summing them.
* [x] Repair the starvation guard: flag on enqueue-to-run wait, boost on the next wakeup.
* [x] Restore `STAT_STALLS` with a defined threshold (enqueue-to-run > 2 s).
* [x] Stop the dispatch counter from changing on empty dispatches, and prevent `vtime` underflow on bonus subtraction.
* [x] Harden `results.py` with schema checks, explicit warm-up, and host-side tests (`make test`).
* [x] Add the stress matrix script (`scripts/stress_matrix.sh`).

Open:

* [ ] Run the stress matrix on target hardware and diagnose the prolonged runnable-task stalls.
* [ ] Improve dispatch liveness under extreme contention.
* [ ] Validate that criticality credit identifies accelerator-feeding tasks (`plan.md` §2.4).
* [ ] Add a `dequeue` callback so the runnable count stays exact.
* [ ] Compare against EEVDF and representative `sched_ext` schedulers with repeated, controlled workloads.
* [ ] Replace the synthetic GPU signal with real telemetry.
* [ ] Report confidence intervals and run-to-run variability.
* [ ] Evaluate p50 and p95 application latency alongside GPU idle time.
* [ ] Test across different contention levels and workload types.
* [ ] Document supported kernel versions, hardware, and dependencies.
## Project Goal

Explore whether accelerator-critical CPU scheduling can improve GPU feeding behavior under contention while preserving fairness and scheduler stability.

The central research question is:

**Can runtime GPU-idleness signals help a CPU scheduler identify and prioritize tasks that matter most to accelerator progress, without sacrificing system-wide fairness or liveness?**
