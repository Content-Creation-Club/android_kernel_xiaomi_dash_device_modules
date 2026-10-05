/* SPDX-License-Identifier: GPL-2.0-only */
/* Stock .text+0x0; identical to the arm64 mte_zero_clear_page_tags routine. */
#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/mte-def.h>
#include <asm/page.h>

	.arch armv8.5-a+memtag
SYM_FUNC_START(mempool_mte_zero_clear_page_tags)
	and x0, x0, #(1 << MTE_TAG_SHIFT) - 1
	mrs x1, dczid_el0
	tbnz x1, #4, 2f
	and w1, w1, #0xf
	mov x2, #4
	lsl x1, x2, x1
1:	dc gzva, x0
	add x0, x0, x1
	tst x0, #(PAGE_SIZE - 1)
	b.ne 1b
	ret
2:	stz2g x0, [x0], #(MTE_GRANULE_SIZE * 2)
	tst x0, #(PAGE_SIZE - 1)
	b.ne 2b
	ret
SYM_FUNC_END(mempool_mte_zero_clear_page_tags)
