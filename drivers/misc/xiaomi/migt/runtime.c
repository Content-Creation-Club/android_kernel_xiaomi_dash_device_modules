// SPDX-License-Identifier: GPL-2.0
/* Stock .text+0x104..0x4e8. Callback is deliberately not registered here. */
#include <linux/bug.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include "migt.h"

struct rq;
DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);
extern u64 fas_sched_clock(void);
extern u64 scale_exec_time(u64 runtime, struct rq *rq);

static DEFINE_SPINLOCK(migt_lock);
static int old_migt_thresh;
static u64 migt_fast_div, migt_monitor_thresh;
DEFINE_PER_CPU(struct migt_game_cpu, migt_game_load_info);

/* .text+0x632c..0x6468: five cumulative categories under each CPU's lock. */
void game_load_update(struct task_struct *task, u64 runtime, unsigned int cpu)
{
	struct migt_game_cpu *info;
	unsigned long flags;
	bool is_game, is_vip, is_super, is_render;
	if (!(game_load_has_init & 1))
		return;
	is_game = game_task(task) != 0;
	is_vip = game_vip_task(task) != 0;
	is_super = game_super_task(task) != 0;
	is_render = is_render_thread(task) != 0;
	BUG_ON(cpu >= 32);
	info = per_cpu_ptr(&migt_game_load_info, cpu);
	spin_lock_irqsave(&info->lock, flags);
	info->all += runtime;
	if (is_game)
		info->game += runtime;
	if (is_super)
		info->super += runtime;
	if (is_vip)
		info->vip += runtime;
	if (is_render)
		info->render += runtime;
	spin_unlock_irqrestore(&info->lock, flags);
}

void migt_hook(struct task_struct *task, int cpu, int event, u64 wallclock, u64 delta_exec)
{
	struct migt_task_state *gt;
	u64 now = fas_sched_clock(), elapsed, runtime, smoothed, div, cap, bucket;
	u32 uid, threshold, generation, i;
	BUG_ON((unsigned int)cpu >= 32);
	gt = get_gt_task(task);
	if (!gt || boost_policy < 2)
		return;
	if (boost_policy == 3)
		game_load_update(task, scale_exec_time(delta_exec, per_cpu_ptr(&runqueues, cpu)), cpu);
	uid = MI_FIELD((u8 *)gt - 0x1a0, u32, 0xbc);
	if ((uid >> 4) < 0x271 || uid != last_traced_uid)
		return;
	threshold = migt_thresh;
	generation = sys_migt_count;
	elapsed = now >= migt_last_update_time ? now - migt_last_update_time : 0;
	if (generation == GT_FIELD(gt, u32, 0)) {
		runtime = GT_FIELD(gt, u64, 0x38);
		div = migt_fast_div;
	} else {
		if (!old_migt_thresh || old_migt_thresh != migt_thresh) {
			s64 product;
			old_migt_thresh = migt_thresh;
			migt_fast_div = (s64)(migt_thresh ? 10000 / migt_thresh : 0);
			product = (s64)((u64)((s64)migt_thresh * 1000000) * (u32)migt_viptask_thresh);
			migt_monitor_thresh = product / 100;
		}
		if (delta_exec > elapsed) {
			GT_FIELD(gt, u64, 0x38) += delta_exec - elapsed;
			delta_exec = elapsed;
		}
		runtime = GT_FIELD(gt, u64, 0x38);
		cap = (u64)(threshold << 1) * 1000000;
		smoothed = min(runtime, cap);
		if (GT_FIELD(gt, u64, 0x30))
			smoothed = (smoothed + GT_FIELD(gt, u64, 0x30)) >> 1;
		GT_FIELD(gt, u64, 0x30) = smoothed;
		if (runtime > migt_monitor_thresh) {
			if (fps_mexec_update_policy == 1)
				GT_FIELD(gt, u64, 0x40) = (GT_FIELD(gt, u64, 0x40) + runtime) >> 1;
			else if (!fps_mexec_update_policy)
				GT_FIELD(gt, u64, 0x40)++;
			else if (GT_FIELD(gt, u64, 0x40) < runtime)
				GT_FIELD(gt, u64, 0x40) = runtime;
		}
		if (!migt_fast_div)
			migt_fast_div = threshold ? 10000U / threshold : 0;
		div = migt_fast_div;
		bucket = (u32)((div * runtime) / 1000000000) % 10;
		/* Cumulative bucket counters: stock increments zero through bucket. */
		for (i = 0; i <= bucket; i++)
			GT_FIELD(gt, u32, 0x90 + i * 4)++;
		GT_FIELD(gt, u32, 0) = generation;
		runtime = 0;
	}
	runtime += delta_exec;
	GT_FIELD(gt, u64, 0x38) = runtime;
	if (!div) {
		div = (u32)migt_thresh ? 10000U / (u32)migt_thresh : 0;
		migt_fast_div = div;
	}
	bucket = (u32)((div * runtime) / 1000000000) % 10;
	if ((u32)((u32)fps_variance_ratio * GT_FIELD(gt, u32, 0x90 + bucket * 4)) <
	    GT_FIELD(gt, u32, 0x90) && !migt_boost_flag && game_ip_task(task) &&
	    runtime > migt_monitor_thresh && !affinity_only) {
		if (spin_trylock(&migt_lock)) {
			if (!migt_boost_flag)
				vtask_boost_flag = true;
			migt_boost_flag = true;
			spin_unlock(&migt_lock);
		}
		if (vtask_boost_flag && !work_pending(&irq_boost_work) && !hrtimer_active(&vtask_timer))
			hrtimer_start_range_ns(&vtask_timer, 0, 0, HRTIMER_MODE_REL);
	}
}
