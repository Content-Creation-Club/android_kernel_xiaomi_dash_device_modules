/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_COLDSTART_POLICY_H
#define _XIAOMI_METIS_COLDSTART_POLICY_H
#include "metis_wakeup.h"
/* Actual scheduler policy owner; defaults/controller remain unreconstructed. */
extern u32 stask_prefer_cpu;
bool rq_deadloop_detect(struct rq *rq);
bool metis_coldstart_skip_thiscpu_rt(struct task_struct *task, int cpu);
bool metis_coldstart_spread_rt_task(struct task_struct *task);
/* Fourth callback argument is unused by stock but required by its caller. */
bool metis_coldstart_skip_lb(struct task_struct *task, int source_cpu,
		int destination_cpu, int capacity_delta);
void metis_coldstart_sched_balance_rt(void *unused, struct rq *rq,
		struct task_struct *task, int *done);
/* rt_find_lowest_rq has an unresolved ignored-argument contract; no guessed
 * prototype/body is supplied here. See COLDSTART_POLICY_CONTRACT.txt. */
#endif
