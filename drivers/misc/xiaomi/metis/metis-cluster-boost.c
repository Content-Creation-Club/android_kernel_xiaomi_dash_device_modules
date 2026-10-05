// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL source: sixteen stock cluster boost/controller bodies.
 * No state owners, initialization, fboost worker or module wiring here. */
#include <linux/cpufreq.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include "kernel/sched/sched.h"
#include "metis_cluster_boost.h"
#include "metis_extra_controls.h"

static __always_inline void metis_cluster_bug(void)
{
	asm volatile("brk #0x5512");
	__builtin_unreachable();
}

static __always_inline void metis_cluster_bounds_trap(void)
{
	/* Stock compiler bounds traps are distinct from the explicit BUG. */
	asm volatile("brk #0x1");
	__builtin_unreachable();
}

void boost_cluster(int cluster, int mode, int action, int duration_ms)
{
	struct metis_cluster_boost_timer *timer;
	unsigned long flags;

	if ((u32)cluster >= 4)
		metis_cluster_bug();
	if (!mi_freq_enable)
		return;
	if ((u32)cluster >= 3)
		metis_cluster_bounds_trap();
	bcluster_stat[cluster].mode = mode;
	bcluster_stat[cluster].action = action;
	if (!duration_ms) {
		if (!fboost_exe_mode.delete)
			fboost_exe_mode.execute = 1;
		raw_spin_lock_irqsave(&fboost_pending_lock, flags);
		fboost_pending = 1;
		raw_spin_unlock_irqrestore(&fboost_pending_lock, flags);
		wake_up_process(fboost_thread);
		return;
	}
	timer = &boost_timers[cluster];
	/* An active timer still receives the preceding bcluster_stat writes. */
	if (hrtimer_active(&timer->timer))
		return;
	timer->mode = bcluster_stat[cluster].mode;
	timer->action = bcluster_stat[cluster].action;
	if (!fboost_exe_mode.delete)
		fboost_exe_mode.execute = 1;
	hrtimer_start_range_ns(&timer->timer, (s64)duration_ms * 1000000,
			       0, HRTIMER_MODE_REL);
}

void restore_boost_cluster(int cluster)
{
	if ((u32)cluster >= 4)
		metis_cluster_bug();
	if (!mi_freq_enable)
		return;
	if ((u32)cluster >= 3)
		metis_cluster_bounds_trap();
	bcluster_stat[cluster].action = 2;
	bcluster_stat[cluster].mode = 1;
	if (fboost_debug)
		pr_info("metis-fboost: restore boost cluster %d, bfreq %d, bcpu %d",
			cluster, 2, 1);
}

void store_boost_cluster(int cluster)
{
	if ((u32)cluster >= 4)
		metis_cluster_bug();
	if (!mi_freq_enable)
		return;
	if ((u32)cluster >= 3)
		metis_cluster_bounds_trap();
	bcluster_stat[cluster].action = 1;
	bcluster_stat[cluster].mode = 1;
	if (fboost_debug)
		pr_info("metis-fboost: store boost cluster %d, bfreq %d, bcpu %d",
			cluster, 1, 1);
}

void speed_touch_store_freq(int index)
{
	unsigned int *const last_cpu[4] = {
		&ui_last_cpu, &ren_last_cpu, &sf_last_cpu, &hwc_last_cpu,
	};

	if ((u32)index < 4)
		store_boost_cluster(cpu_to_cluster(*last_cpu[index]));
}

void speed_touch_restore_freq(int index)
{
	unsigned int *const last_cpu[4] = {
		&ui_last_cpu, &ren_last_cpu, &sf_last_cpu, &hwc_last_cpu,
	};

	if ((u32)index < 4)
		restore_boost_cluster(cpu_to_cluster(*last_cpu[index]));
}

int *get_frmboost_sta_hook(void)
{
	return cluster_states;
}

void del_cluster(void)
{
	unsigned long flags;

	fboost_exe_mode.execute = 0;
	fboost_exe_mode.delete = 1;
	raw_spin_lock_irqsave(&fboost_pending_lock, flags);
	fboost_pending = 1;
	raw_spin_unlock_irqrestore(&fboost_pending_lock, flags);
	wake_up_process(fboost_thread);
}

void trigger_fboost_work(void)
{
	unsigned long flags;

	if (!fboost_exe_mode.delete)
		fboost_exe_mode.execute = 1;
	raw_spin_lock_irqsave(&fboost_pending_lock, flags);
	fboost_pending = 1;
	raw_spin_unlock_irqrestore(&fboost_pending_lock, flags);
	/* Stock repeats this complete lock/write/unlock sequence. */
	raw_spin_lock_irqsave(&fboost_pending_lock, flags);
	fboost_pending = 1;
	raw_spin_unlock_irqrestore(&fboost_pending_lock, flags);
	wake_up_process(fboost_thread);
}

enum hrtimer_restart queue_for_boost_cluster(struct hrtimer *timer)
{
	unsigned long flags;

	/* The native callback argument is unused in the stock body. */
	(void)timer;
	if (fboost_debug)
		pr_err("metis-fboost: metis queue_for_boost_cluster start...%d %llu",
			0x169, (unsigned long long)ktime_to_us(ktime_get()));
	/* CMN w9,w8 tests the wrapped 32-bit sum, rather than two ORs. */
	if ((u32)fboost_exe_mode.execute + (u32)fboost_exe_mode.delete) {
		raw_spin_lock_irqsave(&fboost_pending_lock, flags);
		fboost_pending = 1;
		raw_spin_unlock_irqrestore(&fboost_pending_lock, flags);
		wake_up_process(fboost_thread);
	}
	return HRTIMER_NORESTART;
}

void do_adjust_freq_delayed(struct work_struct *work)
{
	metis_perflock_last_freqs_fn get_freqs = get_perflock_last_freqs_func;
	struct metis_cluster_adjust_work *adjust;
	metis_corectl_boost_fn corectl_boost;
	unsigned int cluster;
	int *frequencies;
	int frequency, previous, index;

	adjust = container_of(to_delayed_work(work),
			      struct metis_cluster_adjust_work, work);
	cluster = adjust->cluster;
	if (mi_switch_enable) {
		if (cluster >= 3)
			metis_cluster_bug();
		cluster_states[cluster] = 0;
		/* These providers are mandatory here; stock has no NULL guards. */
		frequencies = get_freqs();
		frequency = frequencies[0];
		if (frequency || frequencies[1] || frequencies[2]) {
			for (index = 0; index < 3; index++) {
				if (index)
					frequency = frequencies[index];
				do {
					previous = frequency;
					update_perflock_freq_func(index, frequency);
					frequencies = get_perflock_last_freqs_func();
					frequency = frequencies[index];
				} while (previous != frequency);
			}
		}
	}
	metis_update_policy_online((int)cluster, 0, -1);
	corectl_boost = oem_corectl_boost_func;
	if (corectl_boost)
		corectl_boost(cluster, 0);
	/* With mi_switch_enable clear, the provider calls precede this BUG. */
	if (cluster >= 3)
		metis_cluster_bug();
	thermal_limit_freq[cluster] = ULONG_MAX;
	if (fboost_debug)
		pr_err("metis-fboost: metis do_adjust_freq_delayed end...%d %llu",
			0x1a9, (unsigned long long)ktime_to_us(ktime_get()));
}

int cpufreq_notifier_max(struct notifier_block *notifier,
			 unsigned long value, void *data)
{
	struct metis_cluster_max_notifier *owner;

	(void)data;
	owner = container_of(notifier, struct metis_cluster_max_notifier,
			     notifier);
	if (owner->cluster >= 3)
		metis_cluster_bug();
	max_limit_freq[owner->cluster] = value;
	return NOTIFY_DONE;
}

void finish_fboost_workfn(struct work_struct *work)
{
	(void)work;
	cpufreq_unregister_notifier(&init_fboost_notifier,
				    CPUFREQ_POLICY_NOTIFIER);
	pr_err("metis-fboost: finish_fboost_workfn!");
}

bool oem_rvh_try_to_wake_up_success(struct task_struct *task)
{
	unsigned int cluster = get_cluster_type(task_cpu(task));
	bool super = metis_sup_task(task);

	if (super) {
		if (cluster >= 4)
			cluster = 0;
		if (cluster >= 3)
			metis_cluster_bug();
		perf_min_freq[cluster] = user_min_freq[cluster];
	}
	return super;
}

bool oem_should_update_freq(struct cpufreq_policy *policy)
{
	unsigned int cluster = get_cluster_type(policy->cpu);

	if (cluster >= 4)
		cluster = 0;
	if (cluster >= 3)
		metis_cluster_bug();
	return policy->cur < perf_min_freq[cluster];
}

unsigned int get_fboost_duration(void)
{
	s64 start = frame_boost_start;
	s64 begin = vtask_monitor_beg_us;
	s64 end = vtask_monitor_end_us;
	unsigned long duration = 0;
	s64 now_us;

	if (start > begin && begin > end) {
		get_frame_boost_end(&duration);
		now_us = ktime_to_us(ktime_get());
		if ((s64)((u64)now_us - (u64)start - duration * 1000UL) < 0) {
			now_us = ktime_to_us(ktime_get());
			/* Stock rereads only the low 32 bits of frame_boost_start. */
			return ((u32)now_us - (u32)frame_boost_start) / 1000U;
		}
	}
	return 0;
}

unsigned long get_metis_limit(int cpu)
{
	s64 start = frame_boost_start;
	s64 begin = vtask_monitor_beg_us;
	s64 end = vtask_monitor_end_us;
	unsigned long duration = 0;
	unsigned int ui_cluster, cluster;
	s64 now_us;

	if (start > begin && begin > end) {
		get_frame_boost_end(&duration);
		now_us = ktime_to_us(ktime_get());
		if ((s64)((u64)now_us - (u64)start - duration * 1000UL) < 0 &&
		    thermal_break_enable) {
			ui_cluster = cpu_to_cluster(ui_last_cpu);
			cluster = cpu_to_cluster(cpu);
			if (metis_debug) {
				if (cluster >= 3)
					metis_cluster_bug();
				pr_info("metis-fas: ui_c %d cpu_c %d ui_last_cpu %d cpu %d thermal %lu \n",
					ui_cluster, cluster, ui_last_cpu, cpu,
					thermal_limit_freq[cluster]);
			}
			if (ui_cluster != cluster) {
				if (cluster >= 3)
					metis_cluster_bug();
				return thermal_limit_freq[cluster];
			}
		}
	}
	return ULONG_MAX;
}
