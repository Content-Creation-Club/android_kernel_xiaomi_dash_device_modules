/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_STOCK_UTIL_H
#define _XIAOMI_METIS_STOCK_UTIL_H

struct task_struct;

/* New, current-native interface: common.h's mtk_cpu_util_next type.
 * This adapter calls the real opaque stock slot; it does not own or publish
 * a provider. The stock callback's canonical C typedef remains UNKNOWN. */
unsigned long metis_final_stock_util_next(int cpu, struct task_struct *task,
		int destination_cpu, int boost);

#endif
