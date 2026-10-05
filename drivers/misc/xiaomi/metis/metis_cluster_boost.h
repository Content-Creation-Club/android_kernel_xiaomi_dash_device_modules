/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_CLUSTER_BOOST_H
#define _XIAOMI_METIS_CLUSTER_BOOST_H

#include <linux/hrtimer.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "metis_common.h"

struct cpufreq_policy;

/* Stock bcluster_stat: three pairs of 32-bit controller fields. */
struct metis_cluster_boost_state {
	s32 mode;
	s32 action;
};

/* Both halves belong to the single eight-byte fboost_exe_mode owner. */
struct metis_fboost_execution {
	s32 execute;
	s32 delete;
};

/* Stock native hrtimer prefix is 0x48, wrapper stride 0x58. Native
 * members follow this tree's definitions; no stock native offsets are used. */
struct metis_cluster_boost_timer {
	struct hrtimer timer;
	u32 cluster;
	s32 mode;
	s32 action;
};

/* Stock native delayed_work prefix is 0x88, wrapper stride 0x90. */
struct metis_cluster_adjust_work {
	struct delayed_work work;
	u32 cluster;
};

/* Stock native notifier_block prefix is 0x18, wrapper stride 0x20. */
struct metis_cluster_max_notifier {
	struct notifier_block notifier;
	u32 cluster;
	u8 registered;
};

typedef int *(*metis_perflock_last_freqs_fn)(void);
typedef void (*metis_update_perflock_freq_fn)(int cluster, int frequency);
typedef void (*metis_corectl_boost_fn)(unsigned int cluster,
				     unsigned int enable);

/* Actual stock owners and providers. Their registration, initialization,
 * worker lifetime and teardown remain absent; no defaults are supplied. */
extern struct metis_cluster_boost_state bcluster_stat[3];
extern struct metis_fboost_execution fboost_exe_mode;
extern struct metis_cluster_boost_timer boost_timers[3];
extern struct metis_cluster_adjust_work rem_cluster_works[3];
extern struct metis_cluster_max_notifier nb_max_limit[3];
extern raw_spinlock_t fboost_pending_lock;
extern u8 fboost_pending;
extern struct task_struct *fboost_thread;
extern struct notifier_block init_fboost_notifier;
extern struct work_struct finish_fboost_work;
extern s32 mi_freq_enable, mi_switch_enable, fboost_debug;
extern s32 thermal_break_enable;
extern unsigned long max_limit_freq[3], thermal_limit_freq[3];
extern u32 user_min_freq[3];
extern metis_perflock_last_freqs_fn get_perflock_last_freqs_func;
extern metis_update_perflock_freq_fn update_perflock_freq_func;
extern metis_corectl_boost_fn oem_corectl_boost_func;
extern s32 cluster_states[3];
extern unsigned int ui_last_cpu, ren_last_cpu, sf_last_cpu, hwc_last_cpu;
extern s64 frame_boost_start, vtask_monitor_beg_us, vtask_monitor_end_us;

/* Direct stock provider; its body and complete policy ownership are absent. */
void metis_update_policy_online(int cluster, int minimum, int restore_maximum);

/* Four observed 32-bit arguments; duration_ms is proven signed by SMULL.
 * Original typedefs of the first three remain inferred (see contract). */
void boost_cluster(int cluster, int mode, int action, int duration_ms);
void restore_boost_cluster(int cluster);
void store_boost_cluster(int cluster);
void speed_touch_store_freq(int index);
void speed_touch_restore_freq(int index);
int *get_frmboost_sta_hook(void);
void del_cluster(void);
void trigger_fboost_work(void);
enum hrtimer_restart queue_for_boost_cluster(struct hrtimer *timer);
void do_adjust_freq_delayed(struct work_struct *work);
int cpufreq_notifier_max(struct notifier_block *notifier,
			 unsigned long value, void *data);
void finish_fboost_workfn(struct work_struct *work);
bool oem_rvh_try_to_wake_up_success(struct task_struct *task);
bool oem_should_update_freq(struct cpufreq_policy *policy);
unsigned int get_fboost_duration(void);
unsigned long get_metis_limit(int cpu);

#endif
