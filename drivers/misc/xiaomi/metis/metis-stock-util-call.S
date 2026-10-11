/* SPDX-License-Identifier: GPL-2.0-only */
/* New, directly called ABI marshal helper; its address is never the
 * four-argument C callback. Do not attach guessed stock CFI metadata.
 * Stock /metis.ko 00121dac..00121dc4 observes family 0x86f86ddd.
 * gki_defconfig enables CONFIG_CFI_CLANG; the original guard is retained.
 * Native assembly, linkage, KMI/CFI and runtime remain UNTESTED. */
#include <linux/linkage.h>
#include <asm/brk-imm.h>

	.text
SYM_FUNC_START(metis_stock_util_call)
	/* New direct-call input: x0=opaque callback, w1=cpu, x2=task. */
	mov	x8, x0
	mov	w0, w1
	mov	x1, x2
	ldur	w16, [x8, #-4]
	mov	w17, #0x6ddd
	movk	w17, #0x86f8, lsl #16
	cmp	w16, w17
	b.eq	.Lmetis_stock_util_valid
	/* Native traps.c decodes target register in bits 4:0 and expected
	 * type register in bits 9:5: x8, w17, exactly stock BRK #0x8228.
	 * Permissive CFI resumes at the following call, like the original. */
	brk	#(CFI_BRK_IMM_BASE | (17 << 5) | 8)
.Lmetis_stock_util_valid:
	/* Tail call leaves LR/SP/x18 and all callee-saved registers unchanged.
	 * BR x16 uses the call-compatible BTI branch class. No stack frame or
	 * shadow-stack entry belongs to this marshal helper. Preserve full x0. */
	mov	x16, x8
	br	x16
SYM_FUNC_END(metis_stock_util_call)
