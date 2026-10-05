// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: two real stock policy/dispatcher bodies, no provider registration. */
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include "metis_walt_policy.h"
#include "metis_extra_controls.h"
#include "metis_coldstart.h"
#include "metis_vip_queue.h"
#include "metis_wakeup.h"

void walt_get_indicies_hook(struct task_struct *task, u32 *order,
			   int *end, int clusters, bool *selected)
{
	struct mi_task_struct *state;
	u32 first = clusters > 2;
	s32 last = (s32)((u32)clusters - 1);
	s64 frame_start;
	unsigned long duration;

	if (!mi_fboost_enable ||
	    (task->prio < 100 && in_coldstart()) ||
	    test_tsk_thread_flag(task, TIF_32BIT))
		return;
	if (force_cluster_sched_enable)
		*end = 0;
	if (in_cold_start_wakeup()) {
		*end = 0;
		if (cold_start_ui_task(task)) {
			*order = (u32)last;
		} else if (mi_link_vip_task(task)) {
			*order = first;
			if (cold_start_ui_sleep())
				*end = 1;
		} else {
			*order = 0;
		}
		goto chose;
	}
	state = get_mi_task_struct(task);
	if (state) {
		if ((s64)(jiffies - MI_FIELD(state, unsigned long, 0x290)) >= 0) {
			state = get_mi_task_struct(task);
			if (state) {
				MI_FIELD(state, s32, 0x298) = -1;
				state->flags_0xc0 &= 0xffdfffffU;
			}
			state = get_mi_task_struct(task);
			if (state) {
				/* Stock does this even when bit20 was already absent. */
				atomic_dec(&mi_dynamic_vip_num);
				state->flags_0xc0 &= 0xffefffffU;
			}
		} else if (state->flags_0xc0 & 0x100000) {
			*order = first;
			*end = last;
			goto chose;
		}
	}
	if (metis_xxx_task(task, 0x100UL)) {
		*order = first;
		frame_start = frame_boost_start;
		if (frame_start > vtask_monitor_beg_us &&
		    vtask_monitor_beg_us > vtask_monitor_end_us) {
			duration = 0;
			get_frame_boost_end(&duration);
			if ((s64)((u64)(ktime_to_ns(ktime_get()) / 1000) -
				  (u64)frame_start - duration * 1000UL) < 0)
				*order = (u32)last;
		}
		*end = last;
		if (stask_prefer_cluster) {
			*order = stask_prefer_cluster;
			*end = 0;
		}
	} else if (metis_xxx_task(task, 0x400UL)) {
		*order = first;
		*end = last;
		if (render_prefer_cluster) {
			*order = render_prefer_cluster;
			*end = 0;
		}
	} else if (metis_xxx_task(task, 0xd00UL)) {
		*order = first;
		*end = last;
		if (vip_prefer_cluster)
			*order = vip_prefer_cluster;
	} else if (mi_binder_vip_task(task)) {
		/* The incoming order is deliberately left unchanged. */
		*end = 1;
	} else if (metis_xxx_task(task, 0x10UL)) {
		*order = 0;
		*end = 0;
	} else {
		return;
	}
chose:
	/* Never clears an incoming true flag on paths without a selection. */
	*selected = true;
}

int metis_ui_and_ui_pipeline_select_rq(struct task_struct *task,
				     struct cpumask *excluded, int flags)
{
	return metis_wakeup_ui_pipeline_select_rq(task, excluded, flags);
}
