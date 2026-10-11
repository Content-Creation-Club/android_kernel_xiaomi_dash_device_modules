// SPDX-License-Identifier: GPL-2.0-only
/* New adapter, not an additional original METIS body. Stock scheduler
 * cpu_util_without 0012d8b8 calls mtk_cpu_util_next(cpu, task, -1, 0).
 * Every recovered placement carrier requests exactly that operation.
 * Native linkage, CFI, provider lifetime and runtime remain UNTESTED. */
#include "metis-final-owners.h"
#include "metis-stock-util.h"

/* Our private direct-call contract, not the stock callback's C prototype.
 * Assembly marshals the opaque address to the observed w0/x1/x0 ABI. */
extern unsigned long metis_stock_util_call(const void *callback, int cpu,
		struct task_struct *task);

unsigned long metis_final_stock_util_next(int cpu, struct task_struct *task,
		int destination_cpu, int boost)
{
	(void)destination_cpu;
	(void)boost;
	/* Original TP policy rereads the slot before every dynamic call.
	 * Keep its unchecked publication/lifetime race, including NULL after
	 * the entry's earlier presence check. No provider is substituted. */
	return metis_stock_util_call(get_cpu_util_without, cpu, task);
}
