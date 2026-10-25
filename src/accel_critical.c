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
	STAT_CPU_SLICE,
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


static int is_ema_stat(__u32 idx) {
	return idx == STAT_CPU_SLICE || idx == STAT_ITER_LATENCY;
}

/* Counters are summed across CPUs. EMA stats are kept per CPU, so they are averaged
 * over the CPUs that have produced a value; summing them would scale with CPU count. */
static void read_stats(struct accel_critical *skel, __u64 *out) {
	int nr_cpus = libbpf_num_possible_cpus();
	__u64 *vals = calloc(nr_cpus, sizeof(__u64));
	int fd = bpf_map__fd(skel->maps.stats);
	for (__u32 i = 0; i < NR_ACCEL_STATS; i++) {
		__u64 sum = 0;
		int n = 0;
		out[i] = 0;
		if (bpf_map_lookup_elem(fd, &i, vals) == 0) {
			for (int c = 0; c < nr_cpus; c++) {
				if (is_ema_stat(i) && vals[c] == 0) continue;
				sum += vals[c];
				n++;
			}
		}
		out[i] = (is_ema_stat(i) && n > 0) ? sum / n : sum;
	}
	free(vals);

	// Reset max_crit_seen for the next window to ensure metrics update
	int crit_fd = bpf_map__fd(skel->maps.max_crit_seen);
	__u32 key = 0;
	__u64 zero = 0;
	bpf_map_update_elem(crit_fd, &key, &zero, BPF_ANY);
}

static __u64 read_gpu_idle_fraction(struct accel_critical *skel) {
	__u64 total_idle = 0;
	__u64 total_elapsed = 0;
	int fd = bpf_map__fd(skel->maps.gpu_metrics);
	__u32 key = 0;
	struct { __u64 idle; __u64 elapsed; } metrics;
	if (bpf_map_lookup_elem(fd, &key, &metrics) == 0) {
		total_idle = metrics.idle;
		total_elapsed = metrics.elapsed;
	}
	if (total_elapsed == 0) return 0;
	return (total_idle * 1000) / total_elapsed;
	}

static __u64 read_runnable_count(struct accel_critical *skel) {
	__u64 count = 0;
	int fd = bpf_map__fd(skel->maps.runnable_tasks);
	__u32 key = 0;
	if (bpf_map_lookup_elem(fd, &key, &count) == 0) {
		return count;
	}
	return 0;
}



static void handle_transition(bool new_state, struct accel_critical *skel) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	u64 now = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	
	current_gpu_state_idle = new_state;
	struct gpu_idle_state gi;
	gi.active = !new_state;
	gi.idle_since_ns = new_state ? now : 0;
	int fd = bpf_map__fd(skel->maps.gpu_idle);
	u32 key = 0;
	bpf_map_update_elem(fd, &key, &gi, BPF_ANY);
}

/* NOTE: the GPU signal is SYNTHETIC. These randomized active/idle intervals stand in for
 * real accelerator telemetry; they do not measure the GPU. */
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
			fprintf(csv_fp, "timestamp,gpu_idle_fraction,cpu_slice_us,iter_latency_us,runnable_tasks\n");
			fflush(csv_fp);
		} else {
			fprintf(stderr, "warning: cannot open metrics.csv: %s\n", strerror(errno));
		}
	}
	
	libbpf_set_print(NULL);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
	
	skel = SCX_OPS_OPEN(accel_ops, accel_critical);
	SCX_OPS_LOAD(skel, accel_ops, accel_critical, uei);
	// link = SCX_OPS_ATTACH(skel, accel_ops, accel_critical);
	
	printf("accel_critical attached. CSV logging: %s. GPU signal: SYNTHETIC. Ctrl-C to exit.\n", csv_fp ? "ON" : "OFF");
	pthread_create(&feeder_thread, NULL, gpu_idle_feeder, skel);
	
	while (!exit_req && !UEI_EXITED(skel, uei)) {
		read_stats(skel, stats);
		__u64 gpu_frac = read_gpu_idle_fraction(skel);
		__u64 runnables = read_runnable_count(skel);
		
		__u64 type_crit[3] = {0};
		int tc_fd = bpf_map__fd(skel->maps.type_crit_sum);
		for (int t = 0; t < 3; t++) {
			bpf_map_lookup_elem(tc_fd, &t, &type_crit[t]);
		}
		
		printf("\rGPU Idle: %llu/1000 | Runnable: %llu | C: %llu/%llu/%llu | Slice: %llu us | Latency: %llu us | Stalls: %llu",
			(unsigned long long)gpu_frac, (unsigned long long)runnables,
			(unsigned long long)type_crit[0], (unsigned long long)type_crit[1], (unsigned long long)type_crit[2],
			(unsigned long long)stats[STAT_CPU_SLICE], (unsigned long long)stats[STAT_ITER_LATENCY],
			(unsigned long long)stats[STAT_STALLS]);
		fflush(stdout);
		
		if (csv_fp) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			fprintf(csv_fp, "%llu,%llu,%llu,%llu,%llu\n",
				(unsigned long long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000),
				(unsigned long long)gpu_frac,
				(unsigned long long)stats[STAT_CPU_SLICE],
				(unsigned long long)stats[STAT_ITER_LATENCY],
				(unsigned long long)runnables);
			fflush(csv_fp);
		}
		sleep(1);
	}
	printf("\n");
	
	if (csv_fp) fclose(csv_fp);
	pthread_cancel(feeder_thread);
	pthread_join(feeder_thread, NULL);
	// bpf_link__destroy(link);
	UEI_REPORT(skel, uei);
	accel_critical__destroy(skel);
	return 0;
}
