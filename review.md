# Project Review: Adaptive CPU Scheduling for ML Workloads

## 🐛 Bug Analysis: Phase 4 GPU Tracking

### Identified Bug: Incorrect GPU Active Fraction & Latency Metrics
There is a logical flaw in how `gpu_active_frac`, `step_time`, and `iter_latency` are tracked in `src/accel_critical.bpf.c`.

1.  **GPU Active Fraction (`gpu_active_frac`)**:
    *   **Issue**: The `gpu_idle_numerator` is incremented based on whether a task *was* delayed while the GPU was idle. However, it is updated inside `accel_running` every time *any* task starts running. This creates a biased metric that reflects the frequency of task switches rather than the actual temporal fraction of GPU idleness.
    *   **Effect**: The fraction is a "per-dispatch" ratio, not a "per-time" ratio. If the system has many short-running tasks, the metric will be skewed.

2.  **Step Time & Iteration Latency**:
    *   **Issue**: `STAT_STEP_TIME` and `STAT_ITER_LATENCY` are accumulated as simple sums (`*step_cnt += step_ns / 1000`).
    *   **Effect**: These stats are monotonically increasing counters of total time, not the *last* measured step time or a moving average. The user-space tool (`accel_critical.c`) reads these as if they are current values, leading to massive, incorrect numbers in the output.

### 🛠️ Proposed Bug Fixes
*   **For `gpu_active_frac`**: Move the numerator/denominator logic to a timer-based update or calculate it based on the actual duration the GPU spent in the `active=0` state vs `active=1` state using the timestamps in `gpu_idle_state`.
*   **For Metrics**: Change `STAT_STEP_TIME` and `STAT_ITER_LATENCY` from accumulators to "last-value" stores or implement a simple Exponential Moving Average (EMA) within the BPF program.

---

## 🚀 Feature Implementation Guide (from `rp.md`)

### 1. Criticality Inference
**Goal**: Identify threads on the accelerator-feeding critical path without instrumentation.
*   **Step 1**: Track per-task `last_runnable_ts` and `last_run_ts`.
*   **Step 2**: In `accel_running`, calculate `wait_ns = now - last_runnable_ts`.
*   **Step 3**: Cross-reference `wait_ns` with the `gpu_idle` map. If the GPU was idle during this window, increment the task's `crit` score.
*   **Step 4**: Implement a decay function (e.g., `crit = crit - (crit >> 7)`) to ensure historical criticality doesn't permanently prioritize a task.

### 2. Contention-Aware Policy
**Goal**: Only prioritize high-criticality tasks when CPU contention is high.
*   **Step 1**: Implement a global "contention" signal (e.g., by tracking the number of runnable tasks in the DSQ).
*   **Step 2**: In `accel_enqueue`, calculate a `vtime` bonus: `bonus = (ts->crit * FACTOR)`.
*   **Step 3**: Scale this bonus by the contention level. If contention is low, `bonus \approx 0`. If high, apply the full `MAX_BONUS`.
*   **Step 4**: Subtract the bonus from the task's `vtime` to move it forward in the DSQ.

### 3. Fairness Guard & Starvation Floor
**Goal**: Prevent high-criticality tasks from completely starving others.
*   **Step 1**: Cap the maximum criticality bonus (`MAX_BONUS`) to ensure no task gets "infinite" priority.
*   **Step 2**: Implement a Starvation Floor: In `accel_enqueue`, if `now - last_runnable_ts > STARVATION_MS`, force-assign a high priority (low `vtime`) regardless of the `crit` score.

### 4. ML-Specific Evaluation
**Goal**: Quantify impact on GPU utilization.
*   **Step 1**: Use the `gpu_idle` map to track ground-truth GPU idleness.
*   **Step 2**: Compare `GPU idle percentage` and `End-to-end throughput` across EEVDF, `scx_lavd`, and `scx_bpfland`.
*   **Step 3**: Measure `Jain's Fairness Index` to ensure co-located noisy neighbors aren't excessively starved.
