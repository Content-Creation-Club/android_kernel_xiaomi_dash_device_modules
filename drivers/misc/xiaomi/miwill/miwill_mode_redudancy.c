/* SPDX-License-Identifier: GPL-2.0-only */
#define MIWILL_PRIVATE_REDUNDANCY
#include "framework.inc"
#include "redudancy.inc"

static int miwill_cnt = 1;
module_param(miwill_cnt, int, 0400);
static int __init miwill_red_init_module(void)
{
	return miwill_mode_register(&red_mode);
}
static void __exit miwill_red_cleanup_module(void)
{
	miwill_mode_unregister(&red_mode);
}
module_init(miwill_red_init_module);
module_exit(miwill_red_cleanup_module);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Minjun Xi <ximinjun@xiaomi.com> ");
MODULE_DESCRIPTION("redudancy mode support for MiWiLL protocol");
