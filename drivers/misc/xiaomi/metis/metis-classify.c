/* SPDX-License-Identifier: GPL-2.0-only */
/* PARTIAL: eleven stock classification bodies; no init, owners or exports. */
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include "metis_classify.h"

bool is_dynamic_vip(struct task_struct *task)
{
	struct mi_task_struct *state;
	if (!task)
		return false;
	state = get_mi_task_struct(task);
	return state && (state->flags_0xc0 & 0x800);
}

bool is_deadline_task(struct mi_task_struct *state)
{
	struct mi_task_struct *leader;
	u32 index;
	if (!state)
		return false;
	index = MI_FIELD(state, u32, 0x2f8);
	if (index == U32_MAX) {
		if (!state->task)
			return false;
		leader = get_mi_task_struct(state->task->group_leader);
		if (!leader)
			return false;
		index = MI_FIELD(leader, u32, 0x2fc);
	}
	return index < 7 && (important_tasks[index][0x18] & 8);
}

bool is_deadline_depend_main_task(struct mi_task_struct *state)
{
	struct mi_task_struct *leader;
	u32 index;
	if (!state)
		return false;
	index = MI_FIELD(state, u32, 0x2f8);
	if (index == U32_MAX) {
		if (!state->task)
			return false;
		leader = get_mi_task_struct(state->task->group_leader);
		if (!leader)
			return false;
		index = MI_FIELD(leader, u32, 0x2fc);
	}
	return index < 7 && (important_tasks[index][0x18] & 16);
}

bool is_low_lt_task(struct mi_task_struct *state)
{
	return state && protect_cs && (state->flags_0xc0 & 0x40000);
}

void mark_task_low_lt(struct mi_task_struct *state)
{
	if (state)
		state->flags_0xc0 |= 0x40000;
}

bool task_in_cs(struct task_struct *task)
{
	struct mi_task_struct *state;
	if (!protect_cs || task->prio < 100)
		return false;
	state = get_mi_task_struct(task);
	return state && (MI_FIELD(state, u32, 0x2b4) & 0x18);
}

bool task_in_cs_protect(struct task_struct *task)
{
	struct mi_task_struct *state;
	u64 stamp, now_us, duration;
	if (!protect_cs)
		return false;
	if (task->prio < 100)
		return false;
	state = get_mi_task_struct(task);
	if (!state || !(MI_FIELD(state, u32, 0x2b4) & 0x18))
		return false;
	/* Stock performs signed ns/1000, then unsigned deadline/age comparisons. */
	now_us = (u64)(ktime_to_ns(ktime_get()) / 1000);
	stamp = MI_FIELD(state, u64, 0x2b8);
	duration = (u64)(s64)cs_protect_dur;
	if (now_us <= stamp + (u64)((s64)cs_protect_dur * 30))
		return now_us - stamp < duration;
	/* Stock looks up state again before mutating it. */
	state = get_mi_task_struct(task);
	if (state) {
		MI_FIELD(state, u64, 0x2b8) = 0;
		MI_FIELD(state, u32, 0x2b4) &= 0xffffffe7;
	}
	return false;
}

bool is_preempted_cs_task(struct task_struct *task)
{
	struct mi_task_struct *initial = get_mi_task_struct(task);
	struct mi_task_struct *state;
	u64 now_us, stamp;
	if (task->tgid == cold_start_app_tgid && task->tgid && protect_cs) {
		/* B.PL/B.MI test subtraction sign bits, including wrapping subtraction. */
		if ((s64)(coldstart_end_jif - coldstart_begin_jif) < 0 &&
			(s64)(jiffies - coldstart_begin_jif - 1000UL) < 0) {
			if (task->prio < 100)
				return false;
			state = get_mi_task_struct(task);
			return state && (MI_FIELD(state, u32, 0x2b4) & 0x18);
		}
		/* Predicate has the observed global reset side effect. */
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
	} else if (!protect_cs) {
		return false;
	}
	if (task->prio < 100)
		return false;
	state = get_mi_task_struct(task);
	if (!state || !(MI_FIELD(state, u32, 0x2b4) & 0x18))
		return false;
	now_us = (u64)(ktime_to_ns(ktime_get()) / 1000);
	stamp = MI_FIELD(state, u64, 0x2b8);
	if (stamp + (u64)((s64)cs_protect_dur * 30) < now_us) {
		state = get_mi_task_struct(task);
		if (state) {
			MI_FIELD(state, u64, 0x2b8) = 0;
			MI_FIELD(state, u32, 0x2b4) &= 0xffffffe7;
		}
		return false;
	}
	if (now_us - stamp < (u64)(s64)cs_protect_dur)
		return initial && MI_FIELD(initial, u8, 0xba) == 0;
	return false;
}

bool is_mmaplk_vip_task(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);
	return state && (MI_FIELD(state, u32, 0x2b4) & 0x800);
}

void set_mmaplk_vip_task(struct mi_task_struct *state)
{
	if (!state)
		return;
	MI_FIELD(state, u32, 0x2b4) |= 0x800;
	if (cs_debug)
		pr_info("metis-cs: mmaplk:%d set mmaplk vip task\n", state->task->pid);
}

void clean_mmaplk_vip_task(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);
	if (!state)
		return;
	MI_FIELD(state, u32, 0x2b4) &= 0xfffff7ff;
	if (cs_debug)
		pr_info("metis-cs: mmaplk:%d clean mmaplk vip task\n", task->pid);
}
