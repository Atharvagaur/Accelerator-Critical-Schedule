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
	u64 ema_step_time;
	u64 ema_iter_latency;
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
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} max_crit_seen SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} vtime_now SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} gpu_idle_fraction SEC(".maps");

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

static __always_inline u64 fold_uclamp(u64 vtime, u64 uclamp_min, u64 slice) {
	s64 delta = (s64)uclamp_min * (s64)slice / (s64)UCLAMP_SCALE;
	if (delta > 0) vtime -= delta;
	return vtime;
}

static __always_inline u64 fold_cgroup_weight(u64 vtime, u32 cgroup_weight, u64 slice) {
	s64 delta = (s64)slice * (CGROUP_WEIGHT_SCALE - cgroup_weight) / (2 * CGROUP_WEIGHT_SCALE);
	vtime -= delta;
	return vtime;
}

s32 BPF_STRUCT_OPS(accel_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags) {
	bool is_idle = false;
	return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
}

void BPF_STRUCT_OPS(accel_enqueue, struct task_struct *p, u64 enq_flags) {
	u32 r_key = 0;
	u64 *r_cnt = bpf_map_lookup_elem(&runnable_tasks, &r_key);
	if (r_cnt) {
		(*r_cnt)++;
		u64 current = *r_cnt;
		u32 m_key = 0;
		u64 *m_cnt = bpf_map_lookup_elem(&max_runnable, &m_key);
		if (m_cnt) {
			if (current > *m_cnt) {
				*m_cnt = current;
			}
		}
	}

	struct task_state *ts = get_task_state(p);
	u64 now = bpf_ktime_get_ns();
	u64 vtime = get_vtime_now();
	u64 actual_enq_flags = enq_flags;

	if (ts) {
		ts->last_runnable_ts = now;
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

			vtime -= (full_bonus * bonus_scale) / 1000;
			stat_inc(STAT_CRIT_CREDIT);
			actual_enq_flags |= SCX_ENQ_PREEMPT;
		}
		vtime = fold_cgroup_weight(vtime, min(ts->cgroup_weight, CGROUP_WEIGHT_SCALE), SCX_SLICE_DFL);
		if (now > ts->last_runnable_ts && (now - ts->last_runnable_ts) > (STARVATION_MS * NSEC_PER_MSEC)) {
			vtime = 0;
			stat_inc(STAT_STARVATION_BOOST);
			actual_enq_flags |= SCX_ENQ_PREEMPT;
			scx_bpf_kick_cpu(-1, SCX_KICK_PREEMPT);
		}
	}
	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, SCX_SLICE_DFL, vtime, actual_enq_flags);
}

void BPF_STRUCT_OPS(accel_dispatch, s32 cpu, struct task_struct *prev) {
	stat_inc(STAT_DISPATCH);
	u32 r_key = 0;
	u64 *r_cnt = bpf_map_lookup_elem(&runnable_tasks, &r_key);
	if (r_cnt && *r_cnt > 0) {
		(*r_cnt)--;
	}

	// Use a local variable for the DSQ to avoid potential verifier issues with the macro
	u32 dsq = SHARED_DSQ;
	if (!scx_bpf_dsq_move_to_local(dsq, 0)) {
		// Truly idle
	}
}

void BPF_STRUCT_OPS(accel_running, struct task_struct *p) {
	struct task_state *ts = get_task_state(p);
	u64 now = bpf_ktime_get_ns();
	if (!ts) return;

	// 1. Step Time: Time since this task last started running
	if (ts->last_run_ts > 0) {
		u64 step_ns = now - ts->last_run_ts;
		u64 val = ts->ema_step_time;
		// EMA: Smooth the interval between scheduling events
		ts->ema_step_time = (val * 7 + (step_ns / 1000)) >> 3;

		// Stall Detection: Log when a task is delayed significantly
		if (step_ns > 2000000000ULL) {
			stat_inc(STAT_STALLS);
		}
	}
	ts->last_run_ts = now;

	// 2. Iteration Latency: Time from enqueue to running
	u64 last_runnable = ts->last_runnable_ts;
	if (last_runnable > 0 && now > last_runnable) {
		u64 latency_ns = now - last_runnable;
		u64 val = ts->ema_iter_latency;
		ts->ema_iter_latency = (val * 7 + (latency_ns / 1000)) >> 3;
	}

	// GPU Criticality Logic
	u64 wait_ns = (now > last_runnable) ? (now - last_runnable) : 0;
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

	// REPORTING: Use a simple EMA for global stats.
	// This eliminates initialization bias and provides a more stable view of system performance.
	u32 s_idx = STAT_STEP_TIME;
	u64 *step_cnt = bpf_map_lookup_elem(&stats, &s_idx);
	if (step_cnt) {
		u64 global_val = *step_cnt;
		u64 task_val = ts->ema_step_time;
		*step_cnt = (global_val * 7 + task_val) >> 3;
	}

	s_idx = STAT_ITER_LATENCY;
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
		u64 dsq_vtime = p->scx.dsq_vtime;
		scx_bpf_task_set_dsq_vtime(p, dsq_vtime + slice);

		u64 last_runnable = ts->last_runnable_ts;
		if (last_runnable > 0 && now > last_runnable) {
			u64 iter_ns = now - last_runnable;
			u32 i_idx = STAT_ITER_LATENCY;
			u64 *iter_cnt = bpf_map_lookup_elem(&stats, &i_idx);
			if (iter_cnt) {
				u64 val = *iter_cnt;
				*iter_cnt = (val * 7 + (iter_ns / 1000)) >> 3;
			}
		}
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
	}
	scx_bpf_task_set_dsq_vtime(p, get_vtime_now());
}

void BPF_STRUCT_OPS(accel_disable, struct task_struct *p) {}

s32 BPF_STRUCT_OPS_SLEEPABLE(accel_init) {
	s32 ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret) return ret;
	set_vtime_now(0);
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
