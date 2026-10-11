Adaptive CPU Scheduling for Machine Learning Workloads
A Proposal and Evaluation Framework Using the Linux sched_ext Scheduler Class
Abstract
Modern machine learning (ML) workloads execute as heterogeneous applications composed of CPU-side stages such as data loading, preprocessing, batching, tokenization, and request handling, together with accelerator-side model execution. When CPU-side work that feeds an accelerator is delayed, the accelerator can wait for data or work, reducing utilization and end-to-end performance. This makes CPU scheduling a potentially important optimization layer under CPU contention.
This project proposes an adaptive CPU scheduler implemented using the Linux sched_ext framework. Recent work has already explored AI-focused sched_ext scheduling, including NUMA- and locality-aware approaches for GPU-accelerated training, while existing sched_ext policies such as scx_bpfland also classify tasks using runtime behavior. Therefore, this project does not claim that AI-aware, workload-aware, or behavior-aware scheduling is new in itself. Instead, it investigates a narrower question: whether runtime behavior can be used to infer which individual CPU threads are likely to be on an accelerator-feeding critical path, and whether those threads can receive bounded scheduling preference only when CPU contention makes their delay harmful.
The proposed scheduler will be compared with Linux EEVDF and representative sched_ext schedulers. Evaluation will use ML-specific metrics such as GPU idle percentage and end-to-end throughput together with latency, CPU utilization, context-switch rate, scheduler overhead, and fairness. The aim is to determine whether this targeted form of accelerator-criticality inference provides measurable benefits without requiring changes to the ML application or framework.
1. Introduction
Machine learning systems rarely run as a single monolithic computation. A typical training or inference pipeline interleaves CPU-bound stages—data loading, decoding, augmentation, tokenization, batching, and request dispatch—with accelerator-bound stages executing on a GPU or other accelerator. If a CPU-side thread that feeds the accelerator is delayed, the accelerator may become idle while waiting for the next unit of work. Thus, CPU scheduling can directly affect accelerator utilization and end-to-end throughput.
Linux's default CPU scheduler, EEVDF, is general-purpose. It schedules tasks according to its scheduling model rather than having application-level knowledge of which CPU threads are currently most important to an accelerator pipeline. Under CPU oversubscription, this creates an opportunity to investigate whether workload-specific scheduling can reduce delays on the CPU-to-accelerator critical path.
The sched_ext framework makes such experimentation practical because it permits custom scheduling policies to be implemented with BPF and loaded dynamically. Existing sched_ext schedulers demonstrate that different policies can target latency, interactivity, load balancing, and other workload properties. The present project builds on this capability but focuses specifically on identifying likely accelerator-critical CPU threads and adapting their scheduling preference to the level of CPU contention.
The project is intentionally positioned as an applied systems and empirical study. It does not claim a new kernel subsystem or a fundamentally new scheduling theory. Its contribution is the combination of runtime accelerator-criticality inference, bounded contention-aware preference, no required application instrumentation, and ML-specific evaluation.
2. Literature Review
2.1 General-Purpose Linux Scheduling: CFS and EEVDF
The Completely Fair Scheduler (CFS) was Linux's default scheduler for many years. EEVDF subsequently became the default Linux scheduler and uses an eligible-virtual-deadline model. These schedulers are general-purpose and are not designed around the specific dependency structure of an ML pipeline in which one CPU thread may directly influence when accelerator work can proceed.
2.2 sched_ext: Programmable Scheduling
sched_ext provides a programmable Linux scheduler class that allows scheduling policies to be implemented using BPF and loaded without rebuilding the kernel. It provides a practical substrate for comparing alternative policies against the normal scheduler.
Existing sched_ext schedulers include policies with different goals. For example, scx_bpfland uses runtime task behavior to distinguish more interactive tasks, while other schedulers focus on latency, load balancing, or broader workload characteristics. This is important for the present project because it means that runtime-behavior-based task classification itself is not a new idea.
2.3 Workload-Aware and Datacenter Scheduling
Prior systems such as ghOSt, Shenango, Caladan, Shinjuku, and Arachne demonstrate that scheduling policies specialized for workload characteristics can improve performance under contention. These systems establish the broader principle that general-purpose scheduling can sometimes be improved by making scheduling decisions aware of workload behavior and performance objectives.
2.4 Recent AI-Focused sched_ext Work
Recent Linux kernel development has also moved closer to the ML setting. In 2026, an AI-focused sched_ext scheduler proposal, scx_ai_numa, targeted GPU-accelerated AI training by improving CPU-side locality and NUMA placement. Its motivation includes avoiding CPU-side delays that can interfere with GPU execution. This work overlaps with the present project's motivation but uses a different primary mechanism: locality, cache behavior, and NUMA-aware placement rather than inferring accelerator-criticality from per-task scheduling behavior.
This recent development changes the project's literature gap. The project should no longer claim that AI-focused sched_ext scheduling is absent. Instead, the relevant question is whether there is additional benefit in identifying accelerator-critical CPU threads from runtime behavior and applying bounded scheduling preference specifically under contention.
2.5 Summary of Prior Techniques
Table 1 summarizes the scheduling techniques and systems discussed above, their core approach, target context, and how each relates to the scheduler proposed in this work.
Table 1. Summary of prior scheduling techniques and systems.
Technique / System
Year
Core Approach
Target Context
Relation to Proposed Work
CFS (Completely Fair Scheduler)
2007
Fair-share scheduling using virtual runtime
General-purpose Linux scheduling
Baseline general-purpose scheduler; no accelerator awareness
EEVDF
2023-24
Eligible-virtual-deadline-based fair scheduling
General-purpose Linux scheduling (current default)
Primary baseline compared against in this study
sched_ext
2024-25
Programmable BPF-based scheduler class
Framework for custom scheduling policies
Implementation substrate used to build the proposed scheduler
scx_bpfland
2024-25
Runtime-behavior-based classification of interactive tasks
Interactive/desktop and mixed workloads
Precedent for behavior-based classification; proposed work applies a similar principle to accelerator criticality instead of interactivity
scx_lavd
2024-25
Latency-aware virtual deadline scheduling
Gaming and low-latency interactive workloads
Used as a representative sched_ext baseline
ghOSt
2021
User-space delegation of kernel scheduling decisions
General datacenter workloads
Establishes feasibility of workload-aware scheduling under contention
Shenango
2019
Fast core reallocation for latency-sensitive tasks
Datacenter latency-sensitive services
Demonstrates benefit of fine-grained, workload-specific CPU allocation
Caladan
2020
Interference mitigation at microsecond timescales
Co-located latency-critical and batch workloads
Motivates contention-aware, bounded prioritization approach
Shinjuku
2019
Preemptive scheduling for tail-latency reduction
Microsecond-scale RPC workloads
Supports case for adaptive, contention-sensitive preemption
Arachne
2018
Core-aware user-level thread management
Multi-core datacenter applications
Related workload-aware scheduling principle at thread granularity
scx_ai_numa
2026
NUMA- and locality-aware placement for GPU training
GPU-accelerated AI training
Closest prior work; addresses CPU-GPU interaction via placement/locality rather than per-task criticality inference


2.6 Gap Identified
The literature therefore establishes four relevant facts: general-purpose scheduling does not explicitly understand application-level accelerator dependencies; sched_ext makes workload-specific scheduling practical; runtime behavior is already used by some sched_ext policies for task classification; and recent AI-focused sched_ext work has begun addressing GPU-accelerated workloads through locality and NUMA-aware mechanisms.
The narrower gap investigated by this project is the combination of these ideas: runtime inference of likely accelerator-critical CPU threads, without requiring explicit application instrumentation, followed by bounded and contention-aware scheduling preference. The study further evaluates whether this specific policy improves GPU idle time and ML throughput while preserving fairness and keeping scheduler overhead low.
This positioning makes the contribution complementary to existing AI-focused and behavior-aware sched_ext work rather than claiming that those areas are unexplored.
3. Problem Statement
Under CPU oversubscription, CPU-side threads involved in an ML pipeline may compete with unrelated CPU-bound workloads. A general-purpose scheduler has no direct application-level indication that delaying one particular preprocessing, data-loading, or request-dispatch thread may cause an accelerator to wait.
At the same time, existing workload-aware schedulers already demonstrate that runtime behavior can be used to classify or prioritize tasks, and recent AI-focused sched_ext work demonstrates that GPU-oriented scheduling can be addressed through mechanisms such as NUMA-aware placement. The open engineering question for this project is therefore narrower: can runtime signals be combined to estimate accelerator criticality at the individual-task level and can a bounded, contention-aware preference reduce accelerator starvation without causing unacceptable fairness or overhead costs?
This leads to three research questions:
Attribution: Under controlled CPU oversubscription, how much GPU idle time and throughput loss is associated with CPU scheduling policy?
Effectiveness: Can runtime inference of accelerator-critical CPU threads reduce GPU idle time and improve throughput compared with general-purpose and representative sched_ext policies, without disproportionately starving competing workloads?
Sensitivity: How does the benefit of accelerator-criticality-based scheduling change as CPU contention and workload type change?
4. Project Objectives
Criticality inference: Design and implement a sched_ext BPF scheduler that estimates whether a task is likely to be on an accelerator-feeding critical path using runtime behavior.
Contention-aware policy: Apply only bounded scheduling preference to high-criticality tasks when measurable CPU contention exists, while preserving normal fairness when contention is low.
Benchmark suite: Evaluate at least one CPU-preprocessing-bound training workload, one CPU-dispatch-bound inference workload, and a small producer-consumer microbenchmark.
Comparative evaluation: Compare the proposed scheduler against Linux EEVDF and at least two representative sched_ext schedulers using throughput, latency, GPU idle percentage, CPU utilization, context-switch rate, scheduler overhead, and fairness.
Adaptivity characterization: Measure how the policy changes as CPU oversubscription increases.
Overhead and fairness accounting: Quantify scheduler overhead and determine whether improvements in accelerator utilization create unacceptable interference for other CPU workloads.
5. Novelty and Innovation
The novelty of this project is deliberately scoped to distinguish it from existing work in workload-aware and AI-oriented scheduling. The project does not claim to introduce the sched_ext framework, AI-aware scheduling, workload-aware scheduling, runtime-based task classification, or CPU–GPU co-scheduling as general concepts. Instead, it investigates a specific scheduling approach that combines these established ideas to address the CPU-to-accelerator dependency in ML workloads.
5.1 Proposed Novel Contribution
The proposed work makes the following contributions:
Accelerator-criticality inference: The scheduler estimates whether an individual CPU task is likely to lie on the CPU-to-accelerator critical path by analysing observable runtime characteristics, such as CPU burst behaviour and blocking or wake-up patterns. This differs from classifying tasks solely according to conventional properties such as interactivity or latency sensitivity.
Application-transparent identification: The proposed approach aims to infer accelerator criticality using OS-level runtime information without requiring explicit instrumentation or modification of the ML application or framework.
Contention-aware bounded prioritization: Rather than assigning permanent high priority to accelerator-related tasks, the scheduler dynamically increases their scheduling preference when CPU contention is sufficiently high for scheduling delays to affect accelerator progress. Under low contention, the scheduler can remain closer to normal fair scheduling behaviour.
ML-specific performance evaluation: The effectiveness of the scheduling policy will be evaluated using ML-system metrics, particularly GPU idle time and end-to-end throughput, alongside conventional operating-system metrics such as latency, CPU utilization, scheduling overhead, context-switch rate, and fairness.
These contributions are intended as an applied systems contribution. The project does not propose a fundamentally new scheduling theory; instead, it investigates whether the proposed combination of runtime accelerator-criticality inference and contention-aware scheduling can provide measurable benefits for ML workloads.
5.2 Scope and Boundaries of the Novelty Claim
To ensure that the contribution is clearly distinguished from prior research, the following aspects are explicitly not claimed as novel:
The sched_ext framework itself.
Workload-aware scheduling as a general concept.
Runtime-based task classification.
AI-oriented CPU/GPU scheduling in general.
NUMA-aware scheduling for AI workloads.
GPU idle time as a performance metric.
The contribution instead lies in investigating whether these established concepts can be combined into an OS-level scheduling policy that estimates accelerator criticality at task granularity and provides bounded scheduling preference under CPU contention.
5.3 Positioning Relative to Closest Prior Work
scx_ai_numa: Recent AI-focused sched_ext development has explored scheduling for GPU-accelerated AI training, with particular emphasis on NUMA locality, cache behaviour, and memory placement. The proposed project addresses a different aspect of the problem by focusing on runtime inference of accelerator-critical CPU tasks and their scheduling priority, rather than primarily optimizing NUMA placement.
scx_bpfland: scx_bpfland demonstrates that runtime task behaviour can be used to distinguish tasks according to characteristics such as interactivity. This provides an important methodological precedent for behaviour-based scheduling. The proposed project applies a similar general principle to a different scheduling objective: estimating whether a task is likely to be contributing to the CPU-to-accelerator critical path.
ghOSt and related workload-aware scheduling systems: Systems such as ghOSt, Shenango, and Caladan demonstrate the performance potential of workload-specific scheduling under contention. However, they target different workload structures and scheduling architectures. The proposed project builds on this broader body of work while focusing specifically on accelerator-dependent ML pipelines within the Linux sched_ext framework.
Overall, the project should therefore be positioned as a focused extension of existing workload-aware scheduling research, rather than as a completely new scheduling paradigm. Its primary research contribution is to determine whether task-level accelerator-criticality inference combined with bounded, contention-aware scheduling preference can measurably improve ML performance without introducing unacceptable fairness or scheduling overhead.
6. Proposed System Design
The scheduler will be implemented as a sched_ext BPF program. It will maintain lightweight per-task state and calculate an estimated accelerator-criticality score.
Signal collection: maintain task-level observations such as CPU burst duration, wake-up frequency, voluntary and involuntary context switches, blocking/off-CPU time, and scheduling delay.
Criticality inference: combine these signals to identify tasks whose behavior is consistent with short CPU-side stages that repeatedly prepare or dispatch accelerator work.
Adaptive prioritization: when contention is low, remain close to normal fair behavior. When contention rises, give high-criticality tasks a bounded scheduling preference.
Fairness guard: cap the preference so CPU-bound co-located workloads continue to receive CPU time.
Safety fallback: retain the normal sched_ext fallback behavior so that the default scheduler can resume if the custom policy is unloaded or fails.

Figure 1. Architecture of the proposed sched_ext-based accelerator-criticality-aware scheduler.
7. Evaluation Methodology
7.1 Workloads
CPU-preprocessing-bound training: an image-classification or vision-transformer training workload with CPU-side data loading, decoding, and augmentation before GPU execution.
CPU-dispatch-bound inference serving: a model server with request preprocessing and dynamic batch assembly on the CPU before GPU execution.
Synthetic microbenchmark: a minimal producer-consumer workload to isolate CPU-to-GPU handoff effects.
7.2 Contention and Oversubscription
CPU oversubscription will be induced by placing configurable CPU-bound noisy-neighbor workloads alongside the ML workloads. The virtual-to-physical CPU ratio can be varied from 1× to 4×. The objective is to determine whether the proposed policy becomes more useful as contention increases.
7.3 Baselines
Linux EEVDF.
scx_lavd.
scx_bpfland.
The baselines are important because they allow the study to distinguish general-purpose scheduling effects from the behavior of existing workload-aware sched_ext policies.
7.4 Metrics
End-to-end ML throughput.
p50 latency.
p99 latency.
GPU idle percentage.
CPU utilization.
Context-switch rate.
Scheduler-induced overhead.
Jain's fairness index.
The most important ML-specific outcome is GPU idle percentage, while throughput determines whether reduced idle time actually translates into useful application performance. Fairness and scheduler overhead prevent an apparent performance improvement from being achieved simply by starving other workloads or consuming excessive CPU resources inside the scheduler.
8. Expected Contributions and Conclusion
This project proposes an adaptive sched_ext CPU scheduling policy for heterogeneous ML workloads. The proposal is intentionally positioned in light of recent work: AI-focused sched_ext scheduling and runtime-behavior-based task classification already exist, so the project does not claim those concepts as new.
The intended contribution is to investigate whether accelerator-criticality can be inferred from OS-visible runtime behavior at the individual-task level and used to provide bounded scheduling preference only under CPU contention. The evaluation will determine whether this approach reduces GPU idle time and improves ML throughput without unacceptable fairness loss or scheduler overhead.
If the experiments show a measurable benefit, the work will provide evidence that task-level accelerator criticality is a useful scheduling signal. If the benefit is small or workload-dependent, that result is also valuable because it identifies the conditions under which this form of adaptive CPU scheduling is not worthwhile.
References
[1] H. Abdel-Wahab and I. Stoica, “Earliest Eligible Virtual Deadline First: A Flexible and Accurate Mechanism for Proportional Share Resource Allocation,” 1995.
[2] Linux Kernel Documentation, “Completely Fair Scheduler (CFS).”
[3] Linux Kernel Documentation, “EEVDF Scheduler.”
[4] Linux Kernel Documentation, “Extensible Scheduler Class (sched_ext).”
[5] J. Humphries et al., “ghOSt: Fast & Flexible User-Space Delegation of Linux Scheduling,” Proc. 28th ACM SIGOPS SOSP, 2021.
[6] A. Ousterhout et al., “Shenango: Achieving High CPU Efficiency for Latency-sensitive Datacenter Workloads,” Proc. 16th USENIX NSDI, 2019.
[7] J. Fried et al., “Caladan: Mitigating Interference at Microsecond Timescales,” Proc. 14th USENIX OSDI, 2020.
[8] K. Kaffes et al., “Shinjuku: Preemptive Scheduling for μsecond-scale Tail Latency,” Proc. 16th USENIX NSDI, 2019.
[9] H. Qin et al., “Arachne: Core-Aware Thread Management,” Proc. 13th USENIX OSDI, 2018.
[10] R. Jain, D. Chiu, and W. Hawe, “A Quantitative Measure of Fairness and Discrimination for Resource Allocation in Shared Computer Systems,” DEC Research Report TR-301, 1984.
[11] sched-ext, “sched_ext Schedulers and Tools,” GitHub repository.
[12] Linux kernel mailing-list / sched_ext development, “scx_ai_numa,” 2026 — AI-focused sched_ext work emphasizing NUMA/locality-aware scheduling for GPU-accelerated AI training.
