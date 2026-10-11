/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_LOWLT_H
#define _XIAOMI_METIS_LOWLT_H
#include <linux/percpu.h>
#include "metis_classify.h"

/* METIS's own .data..percpu object, ELF symbol size 0x280. This is private
 * driver storage, never a replica of a native struct rq. */
struct metis_rq { u8 raw[0x280] __aligned(8); };
DECLARE_PER_CPU_SHARED_ALIGNED(struct metis_rq, metis_runqueues);
/* Original signedness unknown; u32 expresses the observed 32-bit wrap. */
DECLARE_PER_CPU(u32, low_lt_num);
extern s32 low_lt_preempt_thres;
extern s32 low_lt_preempt_thres_cold_start;

void lowlt_list_add_task(struct task_struct *task, unsigned int cpu);
void lowlt_list_del_task(struct task_struct *task, unsigned int cpu);
struct task_struct *lowlt_list_first_task(unsigned int cpu);
bool lowlt_tsk_preempt_normal(struct task_struct *task);
#endif
