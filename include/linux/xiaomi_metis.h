/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_XIAOMI_METIS_H
#define _LINUX_XIAOMI_METIS_H

#include <linux/types.h>

struct cpumask;
struct rq;
struct task_struct;

/* Source-backed native contracts. Rebuild scheduler and METIS together:
 * these consumed-input declarations do not reproduce the unknown stock
 * prototypes, context layout, CFI identities or MODVERSIONS checksums. */
void mi_check_preempt_wakeup_hook(struct rq *rq, struct task_struct *task,
		bool *preempt, bool *nopreempt, bool *skip_mtk_vip);
void metis_coldstart_rt_find_lowest_rq(struct cpumask *mask, int found,
		int *lowest);
int metis_choose_cpu_fastpath(struct task_struct *task,
		unsigned long task_util, struct cpumask *skipped,
		const struct cpumask *excluded, int cluster, int end_index,
		bool *skip);
int metis_tp_choose_cpu_fastpath(struct task_struct *task,
		unsigned long task_util, int cluster, int end_index,
		struct cpumask *allowed);

typedef unsigned long (*metis_cpu_util_without_fn)(int cpu,
		struct task_struct *task);
extern metis_cpu_util_without_fn get_cpu_util_without;
extern struct cpumask ***metis_lb_cpu_array;
/* Scheduler-owned slot; MIGT publishes its genuine int(void) controller. */
extern int (*migt_sched_enable)(void);

bool metis_vip_task(struct task_struct *task);
bool metis_sched_enable(void);
void metis_wakeup_cold_start_skip_ui_cpumask(struct task_struct *task,
		struct cpumask *mask);

#endif
