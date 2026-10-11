# Project Report: Accelerator-Critical CPU Scheduling using sched_ext

## 🎯 Overview
This project implements an adaptive CPU scheduler using the Linux `sched_ext` (BPF) framework. The primary goal is to reduce GPU idle time under CPU contention by identifying and prioritizing "accelerator-critical" threads—those on the critical path to feeding the GPU—without requiring any changes to the ML applications themselves.

## 🚀 Implemented Features

### 1. Temporal GPU Idle-Fraction Tracking
The scheduler implements a high-precision, temporal measurement of GPU idleness.
- **Mechanism**: A user-space "feeder" monitors GPU state transitions and calculates the actual nanoseconds of idleness within sliding 1-second windows.
- **Metric**: The result is published to a BPF map as a fixed-point value (`0-1000`), where `1000` means the GPU was fully idle and `0` means fully active.
- **Verification**: In synthetic tests with a 50/50 active/idle cycle, the scheduler accurately reported `gpu_idle_fraction=500`.

### 2. Contention-Aware Criticality Inference
The scheduler learns which tasks are critical by observing their scheduling behavior.
- **Attribution**: If a task is delayed (wait time > 0) while the GPU is idle, the scheduler credits the task's "criticality score."
- **Contention Scaling**: Priority bonuses are not static. They are scaled by the number of runnable tasks:
  $$\text{Bonus} = \text{Full Bonus} \times \frac{\min(\text{runnable\_tasks}, \text{Threshold})}{\text{Threshold}}$$
- **Behavior**: Under low contention, the scheduler behaves like a fair scheduler. Under high contention, high-criticality tasks receive a bounded priority boost to prevent GPU starvation.

### 3. Stability & Fairness Guards
- **Starvation Floor**: To prevent any task from being completely starved by high-criticality threads, a starvation floor is implemented. Any task waiting longer than $500\text{ms}$ is forced to the front of the queue (`vtime = 0`).
- **EMA Metrics**: Used Exponential Moving Averages (EMA) for `step_time` and `iteration_latency` to provide stable performance metrics without monotonic overflow.

## 📊 Key Findings & Research Observations

### GPU Utilization Improvement
The scheduler successfully identifies critical paths. By boosting threads that correlate with GPU idleness, the system can proactively reduce the time the accelerator spends waiting for data.

### The "Contention Paradox" (Stability Analysis)
During stress testing with `stress-ng`, a significant finding was observed:
- **Observation**: Despite the vtime-based priority and starvation floor, the kernel watchdog occasionally ejected the scheduler due to "runnable task stalls" (tasks waiting $>40\text{s}$).
- **Analysis**: In extreme oversubscription cases, some tasks in specific DSQs were not being dispatched despite having the lowest vtime. This suggests that in highly contended environments, vtime-based preference alone may not be enough to guarantee liveness, and a more aggressive preemption ("kick") mechanism may be required.

## 🛠️ Technical Stack
- **Framework**: Linux `sched_ext` (BPF)
- **Language**: C (User-space), BPF C (Kernel)
- **Metrics**: `metrics.csv` (Timestamp, GPU Idle %, Step Time, Iteration Latency, Runnable Count)
- **Evaluation Tools**: `stress-ng`, `bpftool`, `libbpf`

## 📈 How to Run & Collect Metrics
1. **Build**: `make SCX_DIR=/home/vpekka/scx`
2. **Run**: `sudo ./accel_critical --csv`
3. **Test**: Run `stress-ng --cpu 4` in another terminal to observe the contention-aware scaling in action.
4. **Analyze**: Import `metrics.csv` into Python/Excel to plot GPU Idle % vs. Runnable Tasks.
