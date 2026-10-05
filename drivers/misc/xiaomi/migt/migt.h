/* SPDX-License-Identifier: GPL-2.0 */
/* Unintegrated stock source fragments; see RECONSTRUCTION.txt. */
#ifndef XIAOMI_MIGT_H
#define XIAOMI_MIGT_H
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include "../mi_schedule/mi_schedule.h"

/* Caller-owned suffix of mi_task_struct, beginning at stock offset 0x1a0.
 * Unnamed fields retain offsets. No independent allocation is permitted. */
struct migt_task_state { u8 raw[MI_TASK_STATE_SIZE - 0x1a0] __aligned(8); };
#define GT_FIELD(state, type, offset) MI_FIELD(state, type, offset)
#define GT_FLAGS(state) GT_FIELD(state, u32, 4)

extern int boost_policy, affinity_only, choose_cpu_exclusive_enable;
extern int custom_second_group, force_viptask_select_rq, gtask_override_schedboost;
extern int stask_prefer_cpu, render_prefer_cpu, vip_prefer_cpu, ip_prefer_cpu;
extern int stask_affine_cpu, render_affine_cpu, vip_affine_cpu, ip_affine_cpu;
extern int stask_prefer_cluster, render_prefer_cluster, vip_prefer_cluster, ip_prefer_cluster;
extern u64 migt_reserve_cpus, migt_reserve_cpus_rt, migt_reserve_cpus_mtk_vip;
extern u32 last_traced_uid;
extern int migt_thresh, migt_viptask_thresh, fps_variance_ratio;
extern int fps_mexec_update_policy, sys_migt_count;
extern u64 migt_last_update_time;
extern bool migt_boost_flag, vtask_boost_flag;
/* These real work/timer objects require the omitted stock startup path. */
extern struct work_struct irq_boost_work;
extern struct hrtimer vtask_timer;

extern u8 frame_load_has_init, game_load_has_init;
extern int flw_enable, flw_freq_enable, flw_debug;
extern int glk_disable, glk_fbreak_enable, glk_freq_limit_walt, glk_freq_limit_start;
extern bool glk_maxfreq_limit, glk_minfreq_limit;
extern u8 force_maxfreq_break;
extern u32 glk_maxfreq[3], glk_minfreq[3];
extern int glk_highspeed_load[3], glk_lowspeed_load[3], game_load_debug;
extern int game_load_perf_mod;
extern u32 cur_history_items;
extern u64 game_load_last_update_time;
struct migt_game_history {
	u64 load[3];
	u64 timestamp;
	u64 period;
	u8 raw_28[0x18];
};
extern struct migt_game_history game_load_history[21];
/* Stock .data..percpu+0x148, 0x88 bytes; separate from frame-load load_info. */
struct migt_game_cpu {
	u64 all, all_last, game, game_last, super, super_last;
	u64 vip, vip_last, render, render_last;
	u64 counter_50, counter_58, counter_60, counter_68;
	u8 raw_70[0x10];
	spinlock_t lock;
	u32 cluster;
};
DECLARE_PER_CPU(struct migt_game_cpu, migt_game_load_info);
extern int migt_game_cluster_number;
extern u64 migt_game_cluster_cpumask[3];
extern u32 sysctl_game_load_choose_policy;
extern u64 cpu_util_freq_divisor;

/* Separate stock frame table (.bss+0x4560) and per-CPU .data..percpu+0x98.
 * Unidentified record fields retain byte offsets; no frame updater is invented. */
struct migt_frame_history { u8 raw[0x358] __aligned(8); };
struct migt_frame_cpu {
	u64 counters[7];
	u8 raw_38[0x38];
	u64 previous[7];
	u32 cluster;
	u8 raw_ac[4];
};
extern struct migt_frame_history frame_load_history[21];
DECLARE_PER_CPU(struct migt_frame_cpu, migt_frame_load_info);
extern int migt_frame_cluster_number;
extern u64 migt_frame_cluster_cpumask[3];
extern u32 target_delta, old_target_delta;
extern u64 migt_frame_period_ns;
extern int need_to_base, need_to_update, need_to_record;
extern u32 cur_record_freq[3];
extern u64 last_freq_change_time[3], sum_freq_rt[3];

int migt_enable(void);
int migt_affinity_only(void);
int get_migt_thresh(void);
u32 get_cur_render_uid(void);
bool package_runtime_should_stop(void);
struct migt_task_state *get_gt_task(struct task_struct *task);
unsigned long game_task(struct task_struct *task);
unsigned long game_super_task(struct task_struct *task);
unsigned long game_ssuper_task(struct task_struct *task);
unsigned long game_vip_task(struct task_struct *task);
unsigned long game_vvip_task(struct task_struct *task);
unsigned long game_ip_task(struct task_struct *task);
unsigned int is_render_thread(struct task_struct *task);
bool game_render_task(struct task_struct *task);
bool game_stask_pipeline(struct task_struct *task);
bool game_render_pipeline(struct task_struct *task);
void game_wake_task(struct task_struct *task);
void wake_render(struct task_struct *task);
bool is_frender_sibling(struct task_struct *task);
extern u8 rinfo_back[0x80];
unsigned long is_uuser_set_vip(struct task_struct *task);
unsigned long is_uuser_set_ip(struct task_struct *task);
int migt_choose_exclusive_cpu(struct task_struct *task);
bool migt_skip_cpus(struct cpumask *mask);
bool migt_should_skip_migrate(struct task_struct *task, unsigned int cpu);
bool migt_should_skip_this_cpu_rt(struct task_struct *task, unsigned int cpu);
bool migt_should_skip_this_cpu(unsigned int cpu);
void migt_skip_lowest_rt(struct cpumask *mask);
void migt_skip_mtk_vip_cpus(struct cpumask *mask);
bool migt_should_skip_mtk_affinity_task(void);
bool migt_add_task_to_second(int pid);
bool game_choose_cpu_overutiled(struct task_struct *task);
void update_migt_reserve_cpus(void);
void mi_cpu_coregroup_mask(unsigned int cpu, struct cpumask *mask);
unsigned long game_task_override_schedboost(struct task_struct *task);
u8 flw_update_freq_enable(void);
unsigned long frame_load_enable(void);
unsigned long is_rcal_task(struct task_struct *task, int caller_flag);
void migt_hook(struct task_struct *task, int cpu, int event, u64 wallclock, u64 delta_exec);
void game_load_update(struct task_struct *task, u64 runtime, unsigned int cpu);
void game_load_history_update(void);
void game_load_reset(void);
void frame_load_reset(void);
void set_flw_flag(struct task_struct *task, u32 set_bit, u32 clear_bit);
struct kernel_param;
int set_flw_enable(const char *value, const struct kernel_param *param);
int get_flw_enable(char *buffer, const struct kernel_param *param);
/* Seven machine arguments recovered at the actual sugov producer.
 * Source typedef/KCFI acceptance remains untested; nothing registers this. */
void set_next_freq(void *data, unsigned long util, unsigned long mapped_freq,
		unsigned long max, unsigned long *freq, struct cpufreq_policy *policy,
		bool *need_update);

/* Observed slot ABI, not an integer enable variable. No assignment is made
 * by these fragments because startup/teardown are incomplete. */
extern int (*migt_sched_enable)(void);
#endif
