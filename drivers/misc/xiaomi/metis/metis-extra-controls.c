// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: 24 actual stock bodies. No top-level registration/state defaults. */
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include "kernel/sched/sched.h"
#include "metis_extra_controls.h"
#include "metis_callbacks.h"
#include "metis_coldstart.h"
#include "metis_vip_queue.h"
#include "metis_wakeup.h"

void clean_coldstart_data(void)
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

bool in_coldstart(void)
{
	if (!protect_cs || !cold_start_app_tgid)
		return false;
	if ((s64)(coldstart_end_jif - coldstart_begin_jif) < 0 &&
	    (s64)(jiffies - coldstart_begin_jif - 1000UL) < 0)
		return true;
	clean_coldstart_data();
	return false;
}

bool is_home_tasks(struct task_struct *task)
{
	return task->tgid == home_tgid;
}

bool is_systemui_tasks(struct task_struct *task)
{
	return task->tgid == systemui_tgid;
}

bool is_systemserver_tasks(struct task_struct *task)
{
	return task->tgid == systemserver_tgid;
}

bool is_sf_tasks(struct task_struct *task)
{
	return task->tgid == sf_tgid;
}

void clear_preemption_protect(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	if (state) {
		MI_FIELD(state, u64, 0x2b8) = 0;
		MI_FIELD(state, u32, 0x2b4) &= 0xffffffe7U;
	}
}

void metis_set_ui_pipeline(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	if (state)
		state->flags_0xc0 |= 0x200;
}

void metis_clean_ui_pipeline(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	if (state)
		state->flags_0xc0 &= 0xfffffdffU;
}

bool metis_render_pipeline(struct task_struct *task)
{
	/* The reference body itself is MOV W0,WZR; RET, task* Boolean CFI. */
	return false;
}

int set_prefer_cluster_with_check(const char *value,
				const struct kernel_param *param)
{
	s32 cluster = 0;

	if (sscanf(value, "%d", &cluster) != 1)
		return -EINVAL;
	/* The raw B.HS rejects negative values as well as values >= 3. */
	if ((u32)cluster < 3)
		return param_set_int(value, param);
	pr_err("metis-fas: set prefer cluster error, not allowed: %d\n", cluster);
	return -EINVAL;
}

int set_speed_touch_enable(const char *value, const struct kernel_param *param)
{
	u32 enabled = 0;

	if (sscanf(value, "%u\n", &enabled) != 1)
		return -EINVAL;
	speed_touch_enable = enabled;
	if (!enabled) {
		speed_touch_producer_in_boost = 0;
		speed_touch_consumer_in_boost = 0;
	}
	return 0;
}

int set_trace_motion_event(const char *value, const struct kernel_param *param)
{
	u32 event = 0;

	if (sscanf(value, "%u\n", &event) != 1)
		return -EINVAL;
	trace_motion_event = event;
	if (event == 2)
		touch_up_start = ktime_to_ns(ktime_get()) / 1000;
	return 0;
}

void get_frame_boost_end(unsigned long *duration)
{
	/* Stock LDR W zero-extends the four-byte duration into the eight-byte store. */
	*duration = mi_boost_duration;
}

u8 get_vtask_monitor_status(unsigned long *begin_jiffies)
{
	if (begin_jiffies)
		*begin_jiffies = usecs_to_jiffies((u32)vtask_monitor_beg_us);
	return vtask_monitor;
}

u32 get_thread_type(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (task->policy - 1U < 2)
		return 0;
	state = get_mi_task_struct(task);
	if (state && (state->flags_0xc0 & 0x2c0ae020U))
		return 1;
	return task->pid ? 3 : 2;
}

bool curr_mmaplock_need_prio(void)
{
	/* Unset zero TGIDs can match an idle task; no added gate or expiry. */
	return current->tgid == cold_start_app_tgid ||
	       current->tgid == cold_start_sserver_tgid;
}

void mi_set_rt_binder_free(struct task_struct *task)
{
	if (task && task->prio < 100)
		set_mi_vip_task_common_noresched(task, 0x8000UL, 12);
}

void mi_restore_rt_binder_free(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!task || !(task->__state & 1))
		return;
	state = get_mi_task_struct(task);
	if (!state)
		return;
	/* The reference reads jiffies even though it discards the result. */
	(void)jiffies;
	if (state->flags_0xc0 & 0x8000)
		clean_mi_vip_task_common(task, 0x8000UL);
}

void mi_monitor_hook(struct rq *rq, struct task_struct *task)
{
	u32 cluster;

	if (!get_mi_task_struct(task))
		return;
	cluster = cpu_to_cluster(cpu_of(rq));
	if (!coldstart_app_ui_task(task) && !metis_xxx_task(task, 0x100UL))
		return;
	if (cluster >= 3) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	perf_min_freq[cluster] = 0;
}

void find_energy_hook(struct task_struct *task, bool *is_vip)
{
	if (!*is_vip)
		*is_vip = metis_xxx_task(task, 0xd00UL);
	else
		*is_vip = true;
}

void mi_after_enqueue_task_hook(void *unused, struct rq *rq,
			       struct task_struct *task, int flags)
{
	if (!metis_sched_enable())
		return;
	/* UI has precedence; every populated detector in that family still runs. */
	if (mi_ui_task(task))
		ui_run_cpu = cpu_of(rq);
	else if (mi_render_task(task))
		render_run_cpu = cpu_of(rq);
}

void mi_after_dequeue_task_hook(void *unused, struct rq *rq,
			       struct task_struct *task, int flags)
{
	metis_wakeup_clear_wakeup_task(task, flags);
	if (mi_ui_task(task))
		ui_run_cpu = -1;
	else if (mi_render_task(task))
		render_run_cpu = -1;
}

void mi_enqueue_task_fair_hook(struct rq *rq, struct task_struct *task)
{
	/* Original .text+0xf83c..0xf840 is literally RET; not a missing policy stub. */
}
