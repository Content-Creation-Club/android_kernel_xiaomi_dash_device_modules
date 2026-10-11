/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Observed stock ARM64 entries, with machine inputs marshaled to the
 * reconstructed policies. Unknown original C formal types stay unknown.
 * The type words below were read from each original entry minus four;
 * they are not generated current-native CFI or MODVERSIONS proof.
 * Native SYM_TYPED_FUNC_START supplies the BTI landing pad and, under
 * CONFIG_CFI_CLANG, the observed assembly type word. No C function's
 * compiler-generated type identity is changed by this file.
 * Source only: not assembled, linked, loaded or tested.
 */
#include <linux/cfi_types.h>

	.text

/* Stock001173c0: x1=rq, x2=task, x3=preempt, x4=nopreempt.
 * Its frame is0x30 bytes;00117440 reads [x29+0x40], hence entry
 * SP+0x10 is argument11, the provider-local skip-MTK-VIP output.
 * The ignored original arguments have no declared C types here.
 * 001173bc contains d3 6e 5b 9f, little-endian0x9f5b6ed3.
 */
	.set __kcfi_typeid_mi_check_preempt_wakeup_hook, 0x9f5b6ed3
SYM_TYPED_FUNC_START(mi_check_preempt_wakeup_hook)
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	mov	x3, x4
	ldr	x4, [sp, #0x10]
	b	metis_final_preempt_wakeup_policy
SYM_FUNC_END(mi_check_preempt_wakeup_hook)

/* Stock00125d2c: x1=mask, w2=found, x3=lowest CPU result.
 * Neither x0 nor x4 is consumed. The real stock provider passes zero
 * in x4; that does not establish its C type.
 * 00125d28 contains d2 66 b6 70, little-endian0x70b666d2.
 */
	.set __kcfi_typeid_metis_coldstart_rt_find_lowest_rq, 0x70b666d2
SYM_TYPED_FUNC_START(metis_coldstart_rt_find_lowest_rq)
	mov	x0, x1
	mov	w1, w2
	mov	x2, x3
	b	metis_final_coldstart_lowest_policy
SYM_FUNC_END(metis_coldstart_rt_find_lowest_rq)

/* Stock001194bc: x0=task, x1=provider-owned opaque context, x2=skip.
 * 001194f8 reads the excluded-mask pointer before enable callbacks.
 * Other context fields are read lazily by the policy, at their original
 * stages. Original001194b8 type word: 0xc621b909. */
	.set __kcfi_typeid_metis_choose_cpu_fastpath, 0xc621b909
SYM_TYPED_FUNC_START(metis_choose_cpu_fastpath)
	ldr	x3, [x1, #0x20]
	b	metis_stock_choose_context
SYM_FUNC_END(metis_choose_cpu_fastpath)

/* Stock00121cd8: x0=task, x1=opaque context, x2=allowed mask.
 * 00121cfc reads utilization before classification and presence guards.
 * Cluster/end are deferred through stock-context readers in the policy.
 * Original00121cd4 type word: 0x2cc2dd8f. */
	.set __kcfi_typeid_metis_tp_choose_cpu_fastpath, 0x2cc2dd8f
SYM_TYPED_FUNC_START(metis_tp_choose_cpu_fastpath)
	ldr	x3, [x1]
	b	metis_stock_tp_choose_context
SYM_FUNC_END(metis_tp_choose_cpu_fastpath)
