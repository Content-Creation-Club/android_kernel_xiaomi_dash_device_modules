/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_WALT_POLICY_H
#define _XIAOMI_METIS_WALT_POLICY_H
#include "metis_registry.h"
/* Real parameter-owned policy state, not replacement defaults. */
extern u32 force_cluster_sched_enable, render_prefer_cluster, vip_prefer_cluster;
void walt_get_indicies_hook(struct task_struct *task, u32 *order,
			   int *end, int clusters, bool *selected);
int metis_ui_and_ui_pipeline_select_rq(struct task_struct *task,
				     struct cpumask *excluded, int flags);
#endif
