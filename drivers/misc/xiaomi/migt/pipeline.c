// SPDX-License-Identifier: GPL-2.0
/* Stock pipeline detectors and wake consumers, without dispatcher wiring. */
#include <linux/cred.h>
#include <linux/sched.h>
#include "migt.h"
#include "../metis/metis_state.h"

static void pipeline_lookup(struct task_struct *task)
{
	u32 uid;
	if (!migt_enable())
		return;
	rcu_read_lock();
	uid = __kuid_val(rcu_dereference(task->real_cred)->uid);
	rcu_read_unlock();
	if (uid == get_cur_render_uid())
		get_mi_task_struct(task);
}

/* .text+0x415c..0x41b0, entry CFI0x26f27c13: stock always returns false. */
bool game_stask_pipeline(struct task_struct *task)
{
	pipeline_lookup(task);
	return false;
}

/* .text+0x41f4..0x4248, same entry CFI/type and same observed body. */
bool game_render_pipeline(struct task_struct *task)
{
	pipeline_lookup(task);
	return false;
}

/* .text+0x536c..0x53c4, entry CFI0x1703dde1; incoming x0 is the woken task. */
void wake_render(struct task_struct *task)
{
	struct migt_task_state *gt = get_gt_task(task);
	if (!gt || !(GT_FLAGS(gt) & 0xc0))
		return;
	gt = get_gt_task(current);
	if (gt)
		GT_FIELD(gt, u32, 8) += 1;
}

/* .text+0x5648..0x5738, entry CFI0x1703dde1. Debug gates do not gate FAS. */
void game_wake_task(struct task_struct *task)
{
	struct mi_task_struct *state;
	if (migt_enable() && frame_load_has_init == 1 && flw_enable) {
		get_gt_task(current);
		if (flw_debug == 2)
			pr_info("flw: %16s-%5d set %d\n", current->comm, current->pid, 0x800);
	}
	if (migt_enable() && frame_load_has_init == 1 && flw_enable) {
		get_gt_task(task);
		if (flw_debug == 2)
			pr_info("flw: %16s-%5d set %d\n", task->comm, task->pid, 0x400);
	}
	state = get_mi_task_struct(task);
	if (state)
		mi_set_fas_req(state);
}
