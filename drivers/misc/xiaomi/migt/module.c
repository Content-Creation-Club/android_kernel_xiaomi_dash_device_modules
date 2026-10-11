// SPDX-License-Identifier: GPL-2.0
/* Stock module entry/export metadata; source only, native build untested. */
#include <linux/module.h>
#include "module_lifecycle.h"

EXPORT_SYMBOL_GPL(migt_sched_exit);
EXPORT_SYMBOL_GPL(game_load_exit);
EXPORT_SYMBOL_GPL(game_load_init);
EXPORT_SYMBOL_GPL(migt_sched_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("migt-driver by David");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vip-task detected by David");
