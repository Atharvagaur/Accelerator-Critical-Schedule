/* SPDX-License-Identifier: GPL-2.0 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

#define SHARED_DSQ 0
#define FIXED_SCALE 1024
#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))
#define time_before(a, b) ((s64)((a) - (b)) < 0)
#define NSEC_PER_MSEC 1000000ULL
#define UCLAMP_SCALE 1024
#define CGROUP_WEIGHT_SCALE 1024
#define MAX_BONUS (1000 * 1000000ULL)
#define STARVATION_MS 500
#define CRIT_MAX (1000000LL)
#define CRIT_MIN 0
#define CRIT_TO_VTIME 100
#define STALL_FACTOR 1
#define CRIT_DECAY_FAST 4
#define IDLE_THRESHOLD_NS 500000000ULL // 500ms
#define STARVATION_NS (STARVATION_MS * NSEC_PER_MSEC)
#define STALL_NS 2000000000ULL // enqueue-to-run wait counted as a stall (2 s)
#define RUNNABLE_THRESHOLD 128

struct task_state {
	u64 last_runnable_ts;
	u64 last_run_ts;
	u64 last_run_ts_prev;
	u64 crit;
	u64 last_idle_credit_ts;
	u64 uclamp_min;
	u32 cgroup_weight;
	u16 task_type;
	u32 target_cpu;
	u64 ema_cpu_slice;
	u64 ema_iter_latency;
	bool starved;
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_state);
} task_state SEC(".maps");

struct gpu_idle_state {
	u64 idle_since_ns;
	bool active;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct gpu_idle_state);
} gpu_idle SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} runnable_tasks SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} max_runnable SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 3);
	__type(key, u32);
	__type(value, u64);
	} type_crit_sum SEC(".maps");

struct gpu_metrics_val {
	u64 total_idle_ns;
	u64 total_elapsed_ns;
	u64 last_update_ns;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct gpu_metrics_val);
} gpu_metrics SEC(".maps");

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

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, NR_ACCEL_STATS);
	__type(key, u32);
	__type(value, u64);
} stats SEC(".maps");

static __always_inline void stat_inc(u32 idx) {
	u32 key = idx;
	u64 *cnt = bpf_map_lookup_elem(&stats, &key);
	if (cnt) (*cnt)++;
}

static __always_inline u64 get_vtime_now(void) {
	u32 key = 0;
	u64 *v = bpf_map_lookup_elem(&vtime_now, &key);
	return v ? *v : 0;
}

static __always_inline void set_vtime_now(u64 vtime) {
	u32 key = 0;
	bpf_map_update_elem(&vtime_now, &key, &vtime, BPF_ANY);
}

static __always_inline struct task_state *get_task_state(struct task_struct *p) {
	return bpf_task_storage_get(&task_state, p, NULL, BPF_LOCAL_STORAGE_GET_F_CREATE);
}

static __always_inline u64 sat_sub(u64 a, u64 b) {
	return a > b ? a - b : 0;
}

static __always_inline u64 fold_uclamp(u64 vtime, u64 uclamp_min, u64 slice) {
	s64 delta = (s64)uclamp_min * (s64)slice / (s64)UCLAMP_SCALE;
	if (delta > 0) vtime = sat_sub(vtime, (u64)delta);
	return vtime;
}

static __always_inline u64 fold_cgroup_weight(u64 vtime, u32 cgroup_weight, u64 slice) {
	s64 delta = (s64)slice * (CGROUP_WEIGHT_SCALE - cgroup_weight) / (2 * CGROUP_WEIGHT_SCALE);
	if (delta > 0) return sat_sub(vtime, (u64)delta);
	return vtime - delta;
}

s32 BPF_STRUCT_OPS(accel_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags) {
	bool is_idle = false;
	return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
}

void BPF_STRUCT_OPS(accel_enqueue, struct task_struct *p, u64 enq_flags) {
	u32 r_key = 0;
	u64 *r_cnt = bpf_map_lookup_elem(&runnable_tasks, &r_key);
	if (r_cnt) {
		__sync_fetch_and_add(r_cnt, 1);
		u64 current = *r_cnt;
		u32 m_key = 0;
		u64 *m_cnt = bpf_map_lookup_elem(&max_runnable, &m_key);
		if (m_cnt) {
			u64 old_max = *m_cnt;
			while (current > old_max &&
			       __sync_bool_compare_and_swap(m_cnt, old_max, current) == 0) {
				old_max = *m_cnt;
			}
		}
	}

	struct task_state *ts = get_task_state(p);
	u64 now = bpf_ktime_get_ns();
	u64 vtime = get_vtime_now();
	u64 actual_enq_flags = enq_flags;

	if (ts) {
		ts->last_runnable_ts = now;
		ts->is_runnable = true;
		ts->cgroup_weight = CGROUP_WEIGHT_SCALE;
		ts->task_type = 0;
		u64 uclamp_min = (u64)p->uclamp_req[0].value;
		if (uclamp_min > 0) {
			ts->uclamp_min = uclamp_min;
			vtime = fold_uclamp(vtime, uclamp_min, SCX_SLICE_DFL);
			stat_inc(STAT_UCLAMP_FOLDED);
		}
		if (ts->crit > 0) {
			u64 full_bonus = (ts->crit * CRIT_TO_VTIME) / FIXED_SCALE;
			if (full_bonus > MAX_BONUS) full_bonus = MAX_BONUS;

			// New Logic: Scale bonus by actual GPU idle duration instead of runnable count
			u32 g_key = 0;
			struct gpu_idle_state *gi = bpf_map_lookup_elem(&gpu_idle, &g_key);
			u64 bonus_scale = 1000; // Default to full bonus if GPU state is unknown
			if (gi) {
				if (gi->active) {
					bonus_scale = 0; // GPU is active, no bonus needed
				} else if (gi->idle_since_ns > 0) {
					u64 idle_dur = now - gi->idle_since_ns;
					bonus_scale = (idle_dur * 1000) / IDLE_THRESHOLD_NS;
					if (bonus_scale > 1000) bonus_scale = 1000;
				}
			}

			u64 bonus = (full_bonus * bonus_scale) / 1000;
			if (bonus > 0) {
				vtime = sat_sub(vtime, bonus);
				actual_enq_flags |= SCX_ENQ_PREEMPT;
			}
		}
		vtime = fold_cgroup_weight(vtime, min(ts->cgroup_weight, CGROUP_WEIGHT_SCALE), SCX_SLICE_DFL);
		// Starvation recovery: accel_running() flags a task whose last enqueue-to-run
		// wait exceeded STARVATION_MS. The boost applies to its next wakeup.
		if (ts->starved) {
			ts->starved = false;
			vtime = 0;
			stat_inc(STAT_STARVATION_BOOST);
			actual_enq_flags |= SCX_ENQ_PREEMPT;
		}
	}
	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, SCX_SLICE_DFL, vtime, actual_enq_flags);
}

void BPF_STRUCT_OPS(accel_dispatch, s32 cpu, struct task_struct *prev) {
	stat_inc(STAT_DISPATCH);

	// Use a local variable for the DSQ to avoid potential verifier issues with the macro
	u32 dsq = SHARED_DSQ;
	if (!scx_bpf_dsq_move_to_local(dsq, 0))
		return;

	// Decrement only when a task was moved; empty dispatch calls must not change the count.
	u32 r_key = 0;
	u64 *r_cnt = bpf_map_lookup_elem(&runnable_tasks, &r_key);
	if (r_cnt && *r_cnt > 0) {
		__sync_fetch_and_sub(r_cnt, 1);
	}
}

void BPF_STRUCT_OPS(accel_running, struct task_struct *p) {
	struct task_state *ts = get_task_state(p);
	u64 now = bpf_ktime_get_ns();
	if (!ts) return;

	// CPU slice: the time this dispatch spends on the CPU, measured in accel_stopping().
	
					// Update GPU metrics for temporal active fraction
		u32 m_key = 0;
		struct gpu_metrics_val *metrics = bpf_map_lookup_elem(&gpu_metrics, &m_key);
		if (metrics) {
			u64 prev_update = metrics->last_update_ns;
			u64 delta = (now > prev_update) ? (now - prev_update) : 0;

			__sync_fetch_and_add(&metrics->total_elapsed_ns, delta);

			u32 g_key = 0;
			struct gpu_idle_state *gi = bpf_map_lookup_elem(&gpu_idle, &g_key);
			if (gi && !gi->active && gi->idle_since_ns > 0) {
				u64 idle_dur = (now > gi->idle_since_ns) ? (now - gi->idle_since_ns) : 0;
				u64 effective_idle = min(delta, idle_dur);
				__sync_fetch_and_add(&metrics->total_idle_ns, effective_idle);
			}
			metrics->last_update_ns = now;
		}
ts->last_run_ts = now;

	// Iteration latency: enqueue-to-run wait. This is the only latency definition used.
	u64 last_runnable = ts->last_runnable_ts;
	u64 wait_ns = (last_runnable > 0 && now > last_runnable) ? (now - last_runnable) : 0;
	if (wait_ns > 0) {
		u64 val = ts->ema_iter_latency;
		ts->ema_iter_latency = (val * 7 + (wait_ns / 1000)) >> 3;
	}

	// Stall accounting and starvation flagging use the same enqueue-to-run wait.
	if (wait_ns > STALL_NS)
		stat_inc(STAT_STALLS);
	ts->starved = wait_ns > STARVATION_NS;

	// GPU Criticality Logic
	u32 g_key = 0;
	struct gpu_idle_state *gi = bpf_map_lookup_elem(&gpu_idle, &g_key);
	if (gi) {
		bool active = gi->active;
		u64 idle_since = gi->idle_since_ns;
		bool gpu_idle = (!active && idle_since > 0 && idle_since <= last_runnable);

		if (gpu_idle) {
			u64 credit = (wait_ns * STALL_FACTOR) / FIXED_SCALE;
			u64 current_crit = ts->crit;
			u64 new_crit = current_crit - (current_crit >> CRIT_DECAY_FAST) + credit;
			if (new_crit > CRIT_MAX) new_crit = CRIT_MAX;
			ts->crit = new_crit;
			
		// Accumulate criticality by task type for validation
		u32 type_key = ts->task_type;
		u64 *type_sum = bpf_map_lookup_elem(&type_crit_sum, &type_key);
		if (type_sum) *type_sum += credit;
			stat_inc(STAT_CRIT_CREDIT);
		} else {
			u64 current_crit = ts->crit;
			u64 new_crit = current_crit - (current_crit >> CRIT_DECAY_FAST);
			if (new_crit < CRIT_MIN) new_crit = CRIT_MIN;
			ts->crit = new_crit;
		}
	}

	// Global vtime tracking
	u64 dsq_vtime = p->scx.dsq_vtime;
	u64 current_vtime = get_vtime_now();
	if (current_vtime < dsq_vtime) {
		set_vtime_now(dsq_vtime);
	}

	// REPORTING: fold the per-task iteration-latency EMA into this CPU's global stat slot.
	// Readers average the per-CPU slots (see read_stats() in src/accel_critical.c).
	u32 s_idx = STAT_ITER_LATENCY;
	u64 *iter_cnt = bpf_map_lookup_elem(&stats, &s_idx);
	if (iter_cnt) {
		u64 global_val = *iter_cnt;
		u64 task_val = ts->ema_iter_latency;
		*iter_cnt = (global_val * 7 + task_val) >> 3;
	}
}

void BPF_STRUCT_OPS(accel_stopping, struct task_struct *p, bool runnable) {
	struct task_state *ts = get_task_state(p);
	u64 now = bpf_ktime_get_ns();
	if (!ts) return;

	u64 last_run = ts->last_run_ts;
	if (now > last_run) {
		u64 slice = now - last_run;

		// CPU slice: actual CPU execution time of this dispatch (running() -> stopping()).
		// Excludes off-CPU wait and queueing delay.
		u64 val = ts->ema_cpu_slice;
		ts->ema_cpu_slice = (val * 7 + (slice / 1000)) >> 3;

		// Fold per-task CPU slice into this CPU's global STAT_CPU_SLICE slot.
		u32 s_idx = STAT_CPU_SLICE;
		u64 *slice_cnt = bpf_map_lookup_elem(&stats, &s_idx);
		if (slice_cnt) {
			u64 global_val = *slice_cnt;
			u64 task_val = ts->ema_cpu_slice;
			*slice_cnt = (global_val * 7 + task_val) >> 3;
		}

		u64 dsq_vtime = p->scx.dsq_vtime;
		scx_bpf_task_set_dsq_vtime(p, dsq_vtime + slice);

		ts->last_run_ts_prev = now;

		u64 current_vtime = get_vtime_now();
		set_vtime_now(current_vtime + slice);
	}
}

void BPF_STRUCT_OPS(accel_enable, struct task_struct *p) {
	struct task_state *ts = get_task_state(p);
	if (ts) {
		ts->last_runnable_ts = 0; ts->last_run_ts = 0; ts->last_run_ts_prev = 0;
		ts->crit = 0; ts->uclamp_min = 0; ts->cgroup_weight = CGROUP_WEIGHT_SCALE;
		ts->starved = false;
	}
	scx_bpf_task_set_dsq_vtime(p, get_vtime_now());
}

void BPF_STRUCT_OPS(accel_disable, struct task_struct *p) {
	struct task_state *ts = get_task_state(p);
	if (ts && ts->is_runnable) {
		u32 r_key = 0;
		u64 *r_cnt = bpf_map_lookup_elem(&runnable_tasks, &r_key);
		if (r_cnt && *r_cnt > 0) {
			__sync_fetch_and_sub(r_cnt, 1);
		}
		ts->is_runnable = false;
	}
}

s32 BPF_STRUCT_OPS_SLEEPABLE(accel_init) {
	s32 ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret) return ret;
	set_vtime_now(0);
	
	u32 m_key = 0;
	struct gpu_metrics_val init_metrics = {0, 0, 0};
	bpf_map_update_elem(&gpu_metrics, &m_key, &init_metrics, BPF_ANY);
return 0;
}

void BPF_STRUCT_OPS(accel_exit, struct scx_exit_info *ei) { UEI_RECORD(uei, ei); }

SCX_OPS_DEFINE(accel_ops,
	       .select_cpu	= (void *)accel_select_cpu,
	       .enqueue		= (void *)accel_enqueue,
	       .dispatch	= (void *)accel_dispatch,
	       .running		= (void *)accel_running,
	       .stopping	= (void *)accel_stopping,
	       .enable		= (void *)accel_enable,
	       .disable		= (void *)accel_disable,
	       .init		= (void *)accel_init,
	       .exit		= (void *)accel_exit,
	       .name		= "accel_critical");
