/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_SCHED_MIGT_HOOKS_H
#define _MTK_SCHED_MIGT_HOOKS_H

#include <linux/xiaomi_migt.h>

struct rq;

u64 scale_exec_time(u64 delta, struct rq *rq);

/* Owned by scheduler. Registration mirrors the stock unsynchronized slots. */
extern migt_select_cpu_fn game_migt_select_cpu_hook;
extern migt_skip_cpus_fn game_migt_skip_cpus_hook;
extern migt_task_cpu_fn game_migt_should_skip_this_cpu_rt_hook;
extern migt_task_cpu_fn game_migt_should_skip_migrate_hook;
extern migt_task_cpu_fn game_migt_skip_sched_tick_migrate_hook;
extern migt_cpu_fn game_migt_should_skip_newidle_balance_hook;
extern migt_mask_fn game_migt_skip_lowest_rt_hook;
extern migt_mask_fn game_migt_skip_vip_cpus_hook;

#endif
