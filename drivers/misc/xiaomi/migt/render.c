// SPDX-License-Identifier: GPL-2.0
/* Recovered render writers and user classification; no worker or lock init. */
#include <linux/bug.h>
#include <linux/jiffies.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/string.h>
#include "migt.h"
#include "../metis/metis_state.h"

/* .text+0x3238..0x3298. The second lookup is intentional: stock calls twice. */
void set_render_flag(struct task_struct *task, unsigned int index)
{
	struct migt_task_state *gt = get_gt_task(task);
	struct mi_task_struct *mi;
	if (gt)
		GT_FLAGS(gt) |= 1U << ((index + 6U) & 31);
	mi = get_mi_task_struct(task);
	if (mi)
		mi_set_fas_req(mi);
}

/* .text+0x2ffc..0x305c. Clearing any one role clears the common FAS request. */
void clean_render_flag(struct task_struct *task, unsigned int index)
{
	struct migt_task_state *gt = get_gt_task(task);
	struct mi_task_struct *mi;
	if (gt)
		GT_FLAGS(gt) &= ~(1U << ((index + 6U) & 31));
	mi = get_mi_task_struct(task);
	if (mi)
		mi_clean_fas_req(mi);
}

/* .text+0x2c94..0x2d14. Only bit0 of enabled is consumed. */
void user_set_game_vip(struct task_struct *task, unsigned int enabled)
{
	struct migt_task_state *gt;
	struct mi_task_struct *mi;
	if (!task)
		return;
	gt = get_gt_task(task);
	mi = get_mi_task_struct(task);
	if (!gt)
		return;
	if (enabled & 1) {
		GT_FLAGS(gt) = (GT_FLAGS(gt) & ~8U) | 0x20;
		if (mi)
			mi_set_fas_req(mi);
	} else {
		GT_FLAGS(gt) &= ~0x20U;
		if (mi)
			mi_clean_fas_req(mi);
	}
}

/* .text+0x2d18..0x2d98. VIP and IP are mutually exclusive on enable only. */
void user_set_game_ip(struct task_struct *task, unsigned int enabled)
{
	struct migt_task_state *gt;
	struct mi_task_struct *mi;
	if (!task)
		return;
	gt = get_gt_task(task);
	mi = get_mi_task_struct(task);
	if (!gt)
		return;
	if (enabled & 1) {
		GT_FLAGS(gt) = (GT_FLAGS(gt) & ~0x20U) | 8;
		if (mi)
			mi_set_fas_req(mi);
	} else {
		GT_FLAGS(gt) &= ~8U;
		if (mi)
			mi_clean_fas_req(mi);
	}
}

/* .text+0x2ec0..0x2ffc. No task reference is held beyond this RCU section. */
void reset_render_info(unsigned int index)
{
	struct migt_render_info *info;
	struct task_struct *task;
	int old_pid;
	BUG_ON(index > 1);
	info = &render_info[index];
	write_seqlock(&info->lock);
	info->valid = 0;
	info->update_jiffies = 0;
	old_pid = info->pid;
	info->pid = -1;
	memset(info->comm, 0, sizeof(info->comm));
	write_sequnlock(&info->lock);
	if (old_pid > 0) {
		rcu_read_lock();
		task = find_task_by_vpid(old_pid);
		if (task)
			clean_render_flag(task, index);
		rcu_read_unlock();
	}
}

/* .text+0x3060..0x3238. Same PID leaves validity/name/time/flags untouched. */
void update_render_info(struct task_struct *task, unsigned int index)
{
	struct migt_render_info *info;
	struct task_struct *old_task;
	int old_pid;
	size_t length;
	BUG_ON(index > 1);
	info = &render_info[index];
	write_seqlock(&info->lock);
	if (info->pid == task->pid) {
		write_sequnlock(&info->lock);
		return;
	}
	info->valid = 1;
	set_render_flag(task, index);
	info->valid = 1;
	info->update_jiffies = jiffies;
	old_pid = info->pid;
	memset(info->comm, 0, sizeof(info->comm));
	info->pid = task->pid;
	length = strnlen(task->comm, TASK_COMM_LEN);
	/* Stock calls fortify_panic(__fortify_strlen) for an unterminated comm. */
	BUG_ON(length == TASK_COMM_LEN);
	strncpy(info->comm, task->comm, length);
	write_sequnlock(&info->lock);
	/* Deliberately re-read the live PID after dropping the writer lock. */
	if (old_pid > 0 && old_pid != info->pid) {
		rcu_read_lock();
		old_task = find_task_by_vpid(old_pid);
		if (old_task)
			clean_render_flag(old_task, index);
		rcu_read_unlock();
	}
}

/* .text+0x2c18..0x2c90. Two plain stores, without atomic admission. */
void add_work_to_migt(u32 uid)
{
	if (!migt_has_work_todo && migt_init_sucess) {
		g_work = uid;
		migt_has_work_todo = 1;
		wake_up_process(migt_thread);
	} else if (migt_render_debug) {
		printk("\0013migt-gtc: %s, %d, %d \n", __func__, 234, uid);
	}
}

/* .text+0x5f38..0x5f8c. Stock only looks up render status and discards it. */
/* Fair callback family 0x712bc264 includes the unused rq and flags. */
void game_set_cid(struct rq *unused_rq, struct task_struct *task, int unused_flags)
{
	if (migt_enable() && frame_load_has_init == 1 && flw_enable)
		is_render_thread(task);
}
