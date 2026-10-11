/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

// ... (existing includes)
#include <bpf/bpf.h>
#include <scx/common.h>
#include "accel_critical.bpf.skel.h"

/* Manually define the enum to match BPF to avoid compilation errors in userspace */
enum stat_idx {
	STAT_SELECT_CPU_DIRECT,
	STAT_ENQUEUE,
	STAT_DISPATCH,
	STAT_RUNNING,
	STAT_STOPPING,
	STAT_ENABLE,
	STAT_CRIT_CREDIT,
	STAT_STARVATION_BOOST,
	STAT_UCLAMP_FOLDED,
	STAT_CGROUP_WEIGHT_FOLDED,
	STAT_GPU_ACTIVE_FRAC,
	STAT_STEP_TIME,
	STAT_ITER_LATENCY,
		STAT_STALLS,
	NR_ACCEL_STATS,
};

struct gpu_idle_state {
	__u64 idle_since_ns;
	bool active;
};

#define TASK_TYPE_GENERIC	0
#define TASK_TYPE_GPU_FEEDER	1
#define TASK_TYPE_CPU_NOISE	2

static volatile sig_atomic_t exit_req;

static void sigint_handler(int sig) { exit_req = 1; }


static void read_stats(struct accel_critical *skel, __u64 *out) {
	int nr_cpus = libbpf_num_possible_cpus();
	__u64 *vals = calloc(nr_cpus, sizeof(__u64));
	int fd = bpf_map__fd(skel->maps.stats);
	for (__u32 i = 0; i < NR_ACCEL_STATS; i++) {
		out[i] = 0;
		if (bpf_map_lookup_elem(fd, &i, vals) == 0) {
			for (int c = 0; c < nr_cpus; c++) out[i] += vals[c];
		}
	}
	free(vals);

	// Reset max_crit_seen for the next window to ensure metrics update
	int crit_fd = bpf_map__fd(skel->maps.max_crit_seen);
	__u32 key = 0;
	__u64 zero = 0;
	bpf_map_update_elem(crit_fd, &key, &zero, BPF_ANY);
}

static __u64 read_gpu_idle_fraction(struct accel_critical *skel) {
	__u64 frac = 0;
	int fd = bpf_map__fd(skel->maps.gpu_idle_fraction);
	__u32 key = 0;
	bpf_map_lookup_elem(fd, &key, &frac);
	return frac;
}

static __u64 read_runnable_count(struct accel_critical *skel) {
	__u64 count = 0;
	int fd = bpf_map__fd(skel->maps.max_runnable);
	__u32 key = 0;
	if (bpf_map_lookup_elem(fd, &key, &count) == 0) {
		// Reset max_runnable for the next window to detect peaks
		__u64 zero = 0;
		bpf_map_update_elem(fd, &key, &zero, BPF_ANY);
	}
	return count;
}

static u64 window_start_ns = 0;
static u64 acc_idle_ns = 0;
static u64 last_processed_ns = 0;
static bool current_gpu_state_idle = false;

static void process_until(u64 target_ns, struct accel_critical *skel) {
	while (last_processed_ns < target_ns) {
		u64 window_end = window_start_ns + 1000000000ULL;
		u64 segment_end = (target_ns < window_end) ? target_ns : window_end;
		if (current_gpu_state_idle) {
			u64 start = (last_processed_ns > window_start_ns) ? last_processed_ns : window_start_ns;
			if (segment_end > start) acc_idle_ns += (segment_end - start);
		}
		if (segment_end >= window_end) {
			u64 frac = (acc_idle_ns * 1000) / 1000000000ULL;
			if (frac > 1000) frac = 1000;
			int fd = bpf_map__fd(skel->maps.gpu_idle_fraction);
			u32 key = 0;
			bpf_map_update_elem(fd, &key, &frac, BPF_ANY);
			acc_idle_ns = 0;
			window_start_ns = window_end;
		}
		last_processed_ns = segment_end;
	}
}

static void handle_transition(bool new_state, struct accel_critical *skel) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	u64 now = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	process_until(now, skel);
	current_gpu_state_idle = new_state;
	struct gpu_idle_state gi;
	gi.active = !new_state;
	gi.idle_since_ns = new_state ? now : 0;
	int fd = bpf_map__fd(skel->maps.gpu_idle);
	u32 key = 0;
	bpf_map_update_elem(fd, &key, &gi, BPF_ANY);
}

static void *gpu_idle_feeder(void *arg) {
	struct accel_critical *skel = arg;

	// Pin feeder thread to Core 0 to avoid measurement jitter
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

	// Set real-time priority to prevent preemption by noise tasks
	struct sched_param param;
	param.sched_priority = sched_get_priority_max(SCHED_FIFO);
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	u64 now = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	window_start_ns = now;
	last_processed_ns = now;
	handle_transition(false, skel);
	while (!exit_req) {
		// Use randomized intervals to create a signal for the scheduler to learn from
		// Randomized active period: 100ms to 900ms
		int active_us = 100000 + (rand() % 800000);
		handle_transition(false, skel); // GPU ACTIVE
		usleep(active_us);

		// Randomized idle period: 100ms to 900ms
		int idle_us = 100000 + (rand() % 800000);
		handle_transition(true, skel);  // GPU IDLE
		usleep(idle_us);

		clock_gettime(CLOCK_MONOTONIC, &ts);
		process_until(ts.tv_sec * 1000000000ULL + ts.tv_nsec, skel);
	}
	return NULL;
}

int main(int argc, char **argv) {
	struct accel_critical *skel;
	struct bpf_link *link;
	__u64 stats[NR_ACCEL_STATS];
	pthread_t feeder_thread;
	
	FILE *csv_fp = NULL;
	if (argc > 1 && strcmp(argv[1], "--csv") == 0) {
		csv_fp = fopen("metrics.csv", "w");
		if (csv_fp) {
			fprintf(csv_fp, "timestamp,gpu_idle_fraction,step_time_us,iter_latency_us,runnable_tasks\n");
		}
	}

	libbpf_set_print(NULL);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	skel = SCX_OPS_OPEN(accel_ops, accel_critical);
	SCX_OPS_LOAD(skel, accel_ops, accel_critical, uei);
	link = SCX_OPS_ATTACH(skel, accel_ops, accel_critical);

	printf("accel_critical attached. CSV logging: %s. Ctrl-C to exit.\n", csv_fp ? "ON" : "OFF");
	pthread_create(&feeder_thread, NULL, gpu_idle_feeder, skel);

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		read_stats(skel, stats);
		__u64 gpu_frac = read_gpu_idle_fraction(skel);
		__u64 runnables = read_runnable_count(skel);
		
		printf("\rGPU Idle: %llu/1000 | Runnable: %llu | Step: %llu us | Latency: %llu us | Stalls: %llu",
			(unsigned long long)gpu_frac, (unsigned long long)runnables,
			(unsigned long long)stats[STAT_STEP_TIME], (unsigned long long)stats[STAT_ITER_LATENCY],
			(unsigned long long)stats[STAT_STALLS]);
		fflush(stdout);

		if (csv_fp) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			fprintf(csv_fp, "%llu,%llu,%llu,%llu,%llu\n", 
				(unsigned long long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000),
				(unsigned long long)gpu_frac, 
				(unsigned long long)stats[STAT_STEP_TIME], 
				(unsigned long long)stats[STAT_ITER_LATENCY], 
				(unsigned long long)runnables);
		}
		sleep(1);
	}
	printf("\n");

	if (csv_fp) fclose(csv_fp);
	pthread_cancel(feeder_thread);
	pthread_join(feeder_thread, NULL);
	bpf_link__destroy(link);
	UEI_REPORT(skel, uei);
	accel_critical__destroy(skel);
	return 0;
}
