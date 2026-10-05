// SPDX-License-Identifier: GPL-2.0-only
/* Two observed stock monitor algorithms; no state owner or startup wiring. */
#include <linux/bug.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include "kernel/sched/sched.h"
#include "metis_monitor.h"

/* .text+0x17118..0x171f8. This argument is mi state, not a native task. */
void monitor_task_enqueue(struct mi_task_struct *state)
{
	struct task_struct *task, *leader;
	struct mi_task_struct *leader_state, *saved_leader;
	u8 *record;
	u32 index, remaining;

	if (!state)
		return;
	task = READ_ONCE(state->task);
	if (!task)
		return;
	leader = READ_ONCE(task->group_leader);
	if (!leader)
		return;
	saved_leader = get_mi_task_struct(leader);
	if (!saved_leader)
		return;
	index = MI_FIELD(state, u32, 0x2f8);
	if (index == UINT_MAX) {
		task = READ_ONCE(state->task);
		if (!task)
			return;
		leader_state = get_mi_task_struct(READ_ONCE(task->group_leader));
		if (!leader_state)
			return;
		index = MI_FIELD(leader_state, u32, 0x2fc);
	}
	if (index >= 7)
		return;
	record = important_tasks[index];
	if (!(unsigned long)record || !(record[0x18] & 4))
		return;
	/* Stock re-reads the owner without another task NULL guard here. */
	task = READ_ONCE(state->task);
	leader = READ_ONCE(task->group_leader);
	if (!leader)
		return;
	leader_state = get_mi_task_struct(leader);
	if (!leader_state || !(leader_state->flags_0xc0 & 0x800))
		return;
	/* Only low32 deadline/jiffies participate; the delta is signed. */
	remaining = MI_FIELD(saved_leader, u32, 0x290) - (u32)jiffies;
	if ((s32)remaining >= 1)
		set_mi_dynamic_vip_task(READ_ONCE(state->task), remaining);
}

/* .text+0x152b4..0x1544c: the native rq itself is the single argument. */
void mi_scheduler_tick_hook(struct rq *rq)
{
	struct metis_rq *state;
	struct metis_run_history *history;
	struct mi_task_struct *task_state;
	struct task_struct *task;
	s32 seconds;
	u32 cpu, type;
	u64 now, elapsed, threshold;

	if (!sched_doctor_enable)
		return;
	seconds = normal_running_too_long_thresh;
	task_state = get_mi_task_struct(READ_ONCE(rq->curr));
	cpu = cpu_of(rq);
	if (cpu >= 32) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	if (!task_state)
		return;
	state = per_cpu_ptr(&metis_runqueues, cpu);
	threshold = (u64)((s64)seconds * 1000000000LL);
	now = fas_sched_clock();
	elapsed = now - MI_FIELD(state, u64, 0x8);
	if (doublecyc_debug) {
		printk("\0016metis-doublecyc: run too long warning:");
		task = READ_ONCE(rq->curr);
		printk("\0016metis-doublecyc: cpu: %d pid: %d time: %llu",
			cpu_of(rq), task->pid, elapsed);
	}
	if (elapsed < threshold)
		return;
	task = READ_ONCE(rq->curr);
	/* Both SCHED_FIFO (1) and SCHED_RR (2) select record10; others record11. */
	history = &his_lru_task[(u32)(task->policy - 1) < 2 ? 10 : 11];
	history->pid = task->pid;
	history->running_time = elapsed;
	history->sampled_at = now;
	task = READ_ONCE(rq->curr);
	memcpy(history->comm, task->comm, sizeof(history->comm));
	type = get_thread_type(READ_ONCE(rq->curr));
	if (type == 1) {
		MI_FIELD(state, u32, 0x9c)++;
		if (suspend_vip_enable) {
			MI_FIELD(state, u64, 0x270) = now;
			MI_FIELD(state, u8, 0x268) = 0;
		}
	} else {
		MI_FIELD(state, u32, 0x98 + (u64)type * 4)++;
	}
}
