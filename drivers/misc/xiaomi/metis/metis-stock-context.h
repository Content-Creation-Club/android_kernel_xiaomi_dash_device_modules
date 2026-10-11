/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_STOCK_CONTEXT_H
#define _XIAOMI_METIS_STOCK_CONTEXT_H

#include <linux/types.h>

struct cpumask;
struct task_struct;

/* Private direct-call interfaces for observed stock provider data, not
 * declarations of the original entries or of a native fair.c structure.
 * Assembly readers keep the external context opaque and load fields only
 * at their original policy stages. */
unsigned long metis_stock_context_util(const void *context);
struct cpumask *metis_stock_context_skipped(const void *context);
int metis_stock_context_cluster(const void *context);
int metis_stock_context_end(const void *context);

int metis_stock_choose_context(struct task_struct *task, const void *context,
		bool *skip, const struct cpumask *excluded);
int metis_stock_tp_choose_context(struct task_struct *task, const void *context,
		struct cpumask *allowed, unsigned long task_util);

#endif
