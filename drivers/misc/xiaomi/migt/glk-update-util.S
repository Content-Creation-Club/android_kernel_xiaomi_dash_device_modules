/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stock .text+62c0..62c4 is exactly RET with type word49da40f7 at62bc.
 * No original caller/declaration proves the unused C formal arguments.
 * Keep the actual machine body and observed type identity without inventing
 * a C prototype. This symbol is neither called nor exported by this leaf.
 * Native typed entry supplies its required BTI landing pad. Not assembled,
 * linked, CFI-tested or runtime-tested; see EMPTY_CONTROL_CONTRACT.txt.
 */
#include <linux/cfi_types.h>

	.text
	.set __kcfi_typeid_glk_update_util, 0x49da40f7
SYM_TYPED_FUNC_START(glk_update_util)
	ret
SYM_FUNC_END(glk_update_util)
