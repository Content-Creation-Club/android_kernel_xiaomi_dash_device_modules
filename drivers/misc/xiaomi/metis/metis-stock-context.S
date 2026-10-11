/* SPDX-License-Identifier: GPL-2.0-only */
/* Observed external stock scheduler-owned context field readers. This is
 * not a copy of a native kernel structure. Callers have new, explicit local
 * C contracts; none of these helper addresses is taken. Native assembly,
 * linkage, layout and runtime remain UNTESTED. */
#include <linux/linkage.h>

	.text

/* TP00121cfc: task utilization is loaded before classification/guards. */
SYM_FUNC_START(metis_stock_context_util)
	ldr	x0, [x0]
	ret
SYM_FUNC_END(metis_stock_context_util)

/* Choose001195c4: skipped-mask pointer is loaded after allowed-mask setup. */
SYM_FUNC_START(metis_stock_context_skipped)
	ldr	x0, [x0, #0x8]
	ret
SYM_FUNC_END(metis_stock_context_skipped)

/* TP00121d94: signed starting cluster, after the initial idle early return. */
SYM_FUNC_START(metis_stock_context_cluster)
	ldrsw	x0, [x0, #0x2c]
	ret
SYM_FUNC_END(metis_stock_context_cluster)

/* TP00121fa0: signed ending row is reloaded at each row-end test. */
SYM_FUNC_START(metis_stock_context_end)
	ldrsw	x0, [x0, #0x30]
	ret
SYM_FUNC_END(metis_stock_context_end)
