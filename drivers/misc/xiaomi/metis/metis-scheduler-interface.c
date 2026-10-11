// SPDX-License-Identifier: GPL-2.0-only
/* Native adapters to the recovered algorithms. Only consumed inputs cross
 * the shared interface; scheduler owns the real masks, table and utility
 * callback. Original canonical prototypes remain unknown. */
#include <linux/xiaomi_metis.h>
#include <linux/compiler.h>
#include "metis-final-scheduler.h"

/* Stock scheduler's provider computes mtk_cpu_util_next(cpu, task, -1, 0).
 * The native provider has a genuine typed declaration shared with its
 * consumer. Re-read the slot at every call, retaining the original unchecked
 * lifetime race after the placement entry's earlier presence check. */
static unsigned long cpu_util_without(int cpu, struct task_struct *task,
		int destination_cpu, int boost)
{
	return READ_ONCE(get_cpu_util_without)(cpu, task);
}

void mi_check_preempt_wakeup_hook(struct rq *rq, struct task_struct *task,
		bool *preempt, bool *nopreempt, bool *skip_mtk_vip)
{
	metis_final_preempt_wakeup_policy(rq, task, preempt, nopreempt,
			skip_mtk_vip);
}

void metis_coldstart_rt_find_lowest_rq(struct cpumask *mask, int found,
		int *lowest)
{
	metis_final_coldstart_lowest_policy(mask, found, lowest);
}

int metis_choose_cpu_fastpath(struct task_struct *task,
		unsigned long task_util, struct cpumask *skipped,
		const struct cpumask *excluded, int cluster, int end_index,
		bool *skip)
{
	return metis_final_choose_policy(task, task_util, skipped, excluded,
			cluster, end_index, skip, cpu_util_without);
}

int metis_tp_choose_cpu_fastpath(struct task_struct *task,
		unsigned long task_util, int cluster, int end_index,
		struct cpumask *allowed)
{
	return metis_final_tp_choose_policy(task, task_util, cluster, end_index,
			allowed, cpu_util_without);
}
