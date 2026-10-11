// SPDX-License-Identifier: GPL-2.0-only
/* Genuine METIS per-CPU storage, never a native struct rq replica.
 * Original stock bounds, unsigned arithmetic and history defects retained. */
#include <linux/jiffies.h>
#include <linux/string.h>
#include <sched/sched.h>
#include "metis-final-scheduler.h"

static __always_inline void monitor_bound(bool invalid)
{
	if (invalid) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
}

static struct metis_rq *monitor_rq(unsigned int cpu)
{
	monitor_bound(cpu >= 32);
	return per_cpu_ptr(&metis_runqueues, cpu);
}

static bool monitor_coldstart_expired(void)
{
	return (s64)(coldstart_end_jif - coldstart_begin_jif) >= 0 ||
		(s64)(jiffies - coldstart_begin_jif - 1000UL) >= 0;
}

static void monitor_coldstart_clear(void)
{
	cold_start_app_tgid = 0;
	home_tgid = 0;
	systemui_tgid = 0;
	systemserver_tgid = 0;
	sf_tgid = 0;
	cold_start_app_mmlock = NULL;
	cold_start_sserver_tgid = 0;
	cold_start_sserver_mmlock = NULL;
	home_mmlock = NULL;
	systemui_mmlock = NULL;
	systemserver_mmlock = NULL;
	sf_mmlock = NULL;
}

/* 0011dc24 / .text+0x151fc..0x152b0. Threshold conversion is signed
 * multiplication followed by unsigned comparison, as in stock. */
bool rq_deadloop_detect(struct rq *rq)
{
	struct metis_rq *state;
	u64 now, last;
	s32 threshold = normal_running_too_long_thresh;
	if (!sched_doctor_enable)
		return false;
	state = monitor_rq(cpu_of(rq));
	now = fas_sched_clock();
	last = MI_FIELD(state, u64, 0x8);
	if (available_idle_cpu(cpu_of(rq)))
		return false;
	return (!rq->nr_running || rq->nr_running != rq->rt.rt_nr_running) &&
		now - last >= (u64)((s64)threshold * 1000000000LL);
}

/* 0012409c / .text+0x1b674: actual void(task*) contract, family1703dde1.
 * Current PID, rather than TGID, is compared to cold_start_app_tgid. */
void mi_wake_up_new_task_hook(struct task_struct *task)
{
	if (!task || !current->pid || current->pid != cold_start_app_tgid || !protect_cs)
		return;
	if (monitor_coldstart_expired()) {
		monitor_coldstart_clear();
		return;
	}
	last_ui_forked_pid = task->pid;
	last_ui_forked_jif = jiffies;
}

/* Algorithm: 001173c0 / .text+0xe998..0xea48. The provider passes its
 * skip-MTK-VIP output as machine argument eleven, at entry SP+0x10.
 * Original unused canonical C formals remain UNKNOWN. This five-argument
 * policy has an explicit local contract, not the original entry prototype. */
void metis_final_preempt_wakeup_policy(struct rq *rq, struct task_struct *task,
		bool *preempt, bool *nopreempt, bool *skip_mtk_vip)
{
	u32 old_priority, new_priority;
	prio_sched_check(rq);
	if (!MI_FIELD(monitor_rq(cpu_of(rq)), u8, 0x268)) {
		*preempt = false;
		return;
	}
	old_priority = get_preempt_prio(rq->curr);
	new_priority = get_preempt_prio(task);
	if (old_priority < new_priority)
		*skip_mtk_vip = true;
	else if (new_priority < old_priority) {
		*nopreempt = true;
		*skip_mtk_vip = true;
	}
	*preempt = old_priority < new_priority;
}

/* Algorithm only: 00125d2c / .text+0x1d304..0x1d434. Real scheduler.ko
 * caller passes task,mask,int-found,int*lowest,NULL. Fifth ignored formal
 * type remains UNKNOWN; this carrier is not a guessed original entry. */
void metis_final_coldstart_lowest_policy(struct cpumask *mask, int found,
		int *lowest)
{
	u32 preferred = stask_prefer_cpu;
	u32 selected;
	if (!found || !protect_cs || !cold_start_app_tgid)
		return;
	if (monitor_coldstart_expired()) {
		monitor_coldstart_clear();
		return;
	}
	selected = *lowest;
	preferred = selected == U32_MAX ? stask_prefer_cpu : selected;
	if ((selected == U32_MAX || selected == stask_prefer_cpu) &&
		(s32)stask_prefer_cpu > 0 && preferred < 8) {
		if (selected == preferred) {
			*lowest = -1;
			preferred = stask_prefer_cpu;
		}
		cpumask_clear_cpu(preferred, mask);
	}
}

static u32 history_index(struct metis_rq *state, unsigned int offset)
{
	u32 index = MI_FIELD(state, u32, offset);
	monitor_bound(index > 9);
	return index;
}

static void write_long_run(struct metis_final_long_run *history,
		struct metis_rq *state, unsigned int offset,
		s32 pid, s64 seconds, u64 now)
{
	history[history_index(state, offset)].pid = pid;
	history[history_index(state, offset)].running_seconds = seconds;
	history[history_index(state, offset)].sampled_at = now;
}

static u64 *history_bucket(struct metis_rq *state, u32 index)
{
	/* Stock admits index10, whose bins overlap/outgrow its 0x280 bytes. */
	monitor_bound(index > 10);
	return &MI_FIELD(state, u64, 0xd8 + (unsigned long)index * 0x28);
}

static void replace_history_bucket(struct metis_rq *state, u32 index,
		u64 start, unsigned int type, u64 run)
{
	u64 *bucket = history_bucket(state, index);
	unsigned int i;
	for (i = 0; i < 4; i++) {
		MI_FIELD(state, u64, 0xb8 + i * 8) -= bucket[1 + i];
		bucket[1 + i] = 0;
	}
	bucket[0] = start;
	bucket[1 + type] = run;
}

/* 0011dee8 / .text+0x154c0..0x15c6c. Actual scheduler.ko switch caller
 * passes prev,next,rq. Types use current native rq/task members only. */
void mi_schedule_hook(struct task_struct *prev, struct task_struct *next,
		struct rq *rq)
{
	u64 now = fas_sched_clock(), queued_at, run, end, boundary, first, full;
	u64 *bucket, maximum;
	struct metis_rq *state;
	struct mi_task_struct *task_state;
	u32 cpu = cpu_of(rq), seconds, bin, index, new_index, maximum_class;
	s32 full_count, begin, stop, cursor;
	unsigned int type;
	bool overlong = false;
	if (!sched_doctor_enable)
		return;
	task_state = get_mi_task_struct(next);
	state = monitor_rq(cpu);
	if (task_state && MI_FIELD(task_state, u8, 0xba)) {
		queued_at = MI_FIELD(task_state, u64, 0x280);
		MI_FIELD(task_state, u8, 0xba) = 0;
		seconds = (u32)((now - queued_at) / 1000000000ULL);
		if (next->policy - 1U < 2) {
			if (doublecyc_debug)
				pr_info("metis-doublecyc: pid %d enqueue %llu now %llu diff %d CPU %d \n",
					next->pid, queued_at, now, seconds, cpu);
			if ((s32)seconds >= rt_running_too_long_thresh) {
				MI_FIELD(state, u32, 0x20) = (s32)MI_FIELD(state, u32, 0x20) % 10;
				write_long_run(his_rt_task, state, 0x20, next->pid, (s32)seconds, now);
				MI_FIELD(state, u32, 0x20)++;
			}
		} else {
			task_state = get_mi_task_struct(next);
			if ((task_state && (MI_FIELD(task_state, u32, 0xc0) & 0x2c0ae020)) || !next->pid)
				goto next_history;
			if (doublecyc_debug)
				pr_info("metis-doublecyc: pid %d enqueue_tm %llu now %llu diff %d CPU %d \n",
					next->pid, queued_at, now, seconds, cpu);
			if ((s32)seconds < normal_running_too_long_thresh) {
				if ((s32)seconds < normal_running_too_long_thresh / 3)
					goto next_history;
			} else {
				MI_FIELD(state, u32, 0x1c) = (s32)MI_FIELD(state, u32, 0x1c) % 10;
				write_long_run(his_nor_task, state, 0x1c, next->pid, (s32)seconds, now);
				write_long_run(his_max_task, state, 0x1c,
					MI_FIELD(state, s32, 0x48), MI_FIELD(state, s64, 0x28), now);
				MI_FIELD(state, u32, 0x1c)++;
				overlong = true;
			}
			bin = (s32)seconds < 3 ? seconds : 3;
			monitor_bound(bin > 4);
			MI_FIELD(state, u32, 0xa8 + bin * 4)++;
		}
	}
next_history:
	cpu = cpu_of(rq);
	state = monitor_rq(cpu);
	index = MI_FIELD(state, u32, 0x10);
	monitor_bound(index > 10);
	if (MI_FIELD(state, u8, 0x18)) {
		/* Disassembly0011e5c8 retains w1=cpu across the branch. */
		if (doublecyc_debug)
			pr_info("metis-doublecyc: cpu : %d statistic starting", cpu);
		MI_FIELD(state, u64, 0xd8) = now;
		MI_FIELD(state, u64, 0) = now;
		MI_FIELD(state, u8, 0x18) = 0;
		goto overlong_policy;
	}
	new_index = MI_FIELD(state, u32, 0x14);
	monitor_bound(new_index >= 12);
	run = now - MI_FIELD(state, u64, 0x8);
	his_lru_task[new_index].pid = prev->pid;
	his_lru_task[new_index].running_time = run;
	his_lru_task[new_index].sampled_at = now;
	memcpy(his_lru_task[new_index].comm, prev->comm, TASK_COMM_LEN);
	MI_FIELD(state, u32, 0x14) = (s32)(new_index + 1) % 10;
	if (doublecyc_debug && debug_count < 1000) {
		/* Disassembly0011e100/0011e5d8 retains x5=&history.comm.
		 * Stock prints the absolute clock in its runtime field. */
		pr_info("metis-doublecyc: cpu : %d, index: %d, pid : %d, runtime_ns : %llu, owner : %s",
			cpu, new_index, prev->pid, now, his_lru_task[new_index].comm);
		debug_count++;
	}
	if (prev->policy - 1U < 2) {
		type = 0;
	} else {
		task_state = get_mi_task_struct(prev);
		if (task_state && (MI_FIELD(task_state, u32, 0xc0) & 0x2c0ae020))
			type = 1;
		else
			type = prev->pid ? 3 : 2;
	}
	end = MI_FIELD(state, u64, 0) + run;
	bucket = history_bucket(state, index);
	boundary = bucket[0] + 300000000ULL;
	if (end <= boundary) {
		MI_FIELD(state, u64, 0) = end;
		bucket[1 + type] += run;
		MI_FIELD(state, u64, 0xb8 + type * 8) += run;
		goto overlong_policy;
	}
	first = boundary - MI_FIELD(state, u64, 0);
	full = (end - boundary) / 300000000ULL;
	full_count = (s32)full;
	bucket[1 + type] += first;
	if (!full_count) {
		new_index = index < 9 ? index + 1 : index - 9;
		replace_history_bucket(state, new_index, boundary, type, run - first);
	} else {
		begin = (s32)(index + 1);
		stop = (s32)((u32)begin + (u32)full_count);
		if (stop <= begin) {
			asm volatile("brk #1");
			__builtin_unreachable();
		}
		for (cursor = begin; cursor < stop; cursor++) {
			new_index = cursor % 10;
			replace_history_bucket(state, new_index, boundary, type, 300000000ULL);
			boundary += 300000000ULL;
		}
		new_index = stop % 10;
		replace_history_bucket(state, new_index, boundary, type,
			run - first - (s64)full_count * 300000000LL);
	}
	MI_FIELD(state, u64, 0) = end;
	MI_FIELD(state, u32, 0x10) = new_index;
	MI_FIELD(state, u64, 0xb8 + type * 8) += run;
overlong_policy:
	if (overlong) {
		maximum = MI_FIELD(state, u64, 0xb8);
		maximum_class = 0;
		for (type = 1; type < 4; type++) {
			if (MI_FIELD(state, u64, 0xb8 + type * 8) > maximum) {
				maximum = MI_FIELD(state, u64, 0xb8 + type * 8);
				maximum_class = type;
			}
		}
		MI_FIELD(state, u32, 0x98 + maximum_class * 4)++;
		if (maximum_class == 1 && suspend_vip_enable) {
			MI_FIELD(state, u64, 0x270) = now;
			MI_FIELD(state, u8, 0x268) = 0;
		}
	}
	MI_FIELD(state, u64, 0x8) = now;
}
