/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_FINAL_SCHEDULER_H
#define _XIAOMI_METIS_FINAL_SCHEDULER_H

#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/sysctl.h>
#include "metis_vip_queue.h"
#include "metis_coldstart_policy.h"
#include "metis_walt_policy.h"

/* METIS's own .data..percpu object, ELF symbol size 0x280. This is private
 * driver storage, never a replica of a native struct rq. */
struct metis_rq { u8 raw[0x280] __aligned(8); };

/* .bss+0x15db0/0x15ea0/0x15f90: three 10 x 0x18 histories. */
struct metis_final_long_run {
	s32 pid;
	u32 unknown_04;
	u64 sampled_at;
	s64 running_seconds;
};
extern struct metis_final_long_run his_nor_task[10], his_rt_task[10];
extern struct metis_final_long_run his_max_task[10];
extern s32 rt_running_too_long_thresh;
extern s32 debug_count;

/* True stock objects; owner definitions/defaults are root's integration. */
extern u32 tsched_debug, bug_detect, minor_window_app;
extern s32 vip_task_max_cpu_nr;
extern struct cpumask viptask_use_cpumask;
extern struct cpumask cluster_affinity_cpumask[4], cluster_core_mask[4];
extern u8 cluster_core_init_suc;
extern unsigned long cluster_affinity_uid_mlist[3125];
extern unsigned long cluster_affinity_uid_llist[3125];
extern unsigned long cluster_affinity_uid_blist[3125];
extern struct ctl_table_header *clu_aff_header;
extern struct ctl_table clu_aff_table[2];

/* Current fair.c declares cpu_array as cpumask_t ***; its real stock
 * build_cpu_array publishes that same table to metis_lb_cpu_array. */
extern struct cpumask ***metis_lb_cpu_array;
extern const struct cpumask *metis_lb_cpu_halt_mask;
extern s32 metis_lb_min_possible_cluster_id;
extern u32 metis_lb_debug, metis_tp_debug;
extern s32 mi_viptask_balance;
typedef bool (*metis_final_capacity_fn)(unsigned int first, unsigned int second);
typedef bool (*metis_final_overutilized_fn)(unsigned int cpu);
extern metis_final_capacity_fn metis_lb_check_for_higher_capacity;
extern metis_final_overutilized_fn metis_lb_cpu_overutilized;

/* Shared real .text+0xea48 body exists in metis-vip-queue.c. Root exposes
 * it across source files; this slice does not duplicate its algorithm. */
void prio_sched_check(struct rq *rq);
typedef void (*metis_final_game_wake_fn)(struct task_struct *, unsigned int wake_flags);
extern metis_final_game_wake_fn game_wake_hooks;
int uid_type(unsigned int uid);

int clu_aff_init(void);
void clu_aff_exit(void);
void mi_cpu_coregroup_mask(unsigned int cpu, struct cpumask *mask);
void mi_cpuset_fork(void *unused, struct task_struct *child, bool *inherit);
void mi_sched_setaffinity(void *unused, struct task_struct *task,
		const struct cpumask *mask, bool *skip);
void mi_set_cpus_allowed_comm(void *unused, struct task_struct *task,
		const struct cpumask *mask);
unsigned int unbind_cpu_affinity_task(struct task_struct *task);
bool mi_select_task_rq_hook(struct task_struct *task, struct cpumask *excluded,
		int source_cpu, int sd_flag, int wake_flags, int *cpu);
void mi_schedule_hook(struct task_struct *prev, struct task_struct *next,
		struct rq *rq);
void mi_wake_up_new_task_hook(struct task_struct *task);

int metis_lb_init(void);
void metis_lb_exit(void);
int metis_lb_find_busiest_cpu(unsigned int cpu, int cluster, int *reason);
int metis_lb_pull_tasks(unsigned int destination, unsigned int source,
		struct task_struct *fallback, int *reason);
int find_vip_busiest_similar_cap_cpu(unsigned int destination,
		const struct cpumask *mask, int *reason);
int find_vip_runnable_longest_cpu(unsigned int destination,
		const struct cpumask *mask, int *reason);
int pull_tasks_fast_path(unsigned int destination, unsigned int source,
		struct task_struct *fallback);
void rebalance_vip_tasks(void *unused, struct rq *rq, int *continue_balancing);
unsigned long get_cpu_layered_load(unsigned int cpu);
unsigned long get_cpu_vip_tasks_load(unsigned int cpu);
bool metis_tp_check_cpu_layered_load(unsigned int cpu, int *load);
bool metis_tp_is_vip_task(struct task_struct *task);
int metis_tp_mi_viptask_balance(void);
void metis_tp_fixup_cumulative_runnable_avg_hook(struct rq *rq,
		struct task_struct *task, long delta, long previous_delta,
		int operation, unsigned long total, unsigned long previous_total);
int metis_tp_walt_find_best_target_hook(unsigned int cpu, unsigned long *load);
int metis_tp_init(void);
void metis_tp_exit(void);

/* These are only algorithm carriers, not invented stock entry prototypes.
 * Stock preemption has unresolved unused C formals; its eleventh machine
 * argument is the provider's skip-MTK-VIP output. RT-lowest's unused fifth
 * C formal is unknown. These local prototypes describe consumed policy inputs. */
void metis_final_preempt_wakeup_policy(struct rq *rq, struct task_struct *task,
		bool *preempt, bool *nopreempt, bool *skip_mtk_vip);
void metis_final_coldstart_lowest_policy(struct cpumask *mask, int found,
		int *lowest);

/* Explicit carrier interface, NOT the original get_cpu_util_without ABI.
 * Signature is the actual current native common.h mtk_cpu_util_next API;
 * caller must supply that real provider, with no callback cast. Stock
 * get_cpu_util_without family86f86ddd/native private context remain open. */
typedef unsigned long (*metis_final_native_util_fn)(int cpu,
		struct task_struct *task, int destination_cpu, int boost);
int metis_final_tp_choose_policy(struct task_struct *task,
		unsigned long task_util, int cluster, int end_index,
		struct cpumask *allowed, metis_final_native_util_fn native_util);
int metis_final_choose_policy(struct task_struct *task,
		unsigned long task_util, struct cpumask *skipped,
		const struct cpumask *excluded, int cluster, int end_index,
		bool *skip, metis_final_native_util_fn native_util);
#endif
