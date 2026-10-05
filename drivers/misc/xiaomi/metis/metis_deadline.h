/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_DEADLINE_H
#define _XIAOMI_METIS_DEADLINE_H

#include "metis_lowlt.h"

/* Stock .data+0x4018. Its parameter/controller owner remains absent. */
extern u32 deadline_enable;

struct task_struct *metis_deadline_pick_next_task(unsigned int cpu);
void metis_deadline_enqueue_task(unsigned int cpu, struct task_struct *task);
void metis_deadline_dequeue_task(unsigned int cpu, struct task_struct *task);
void metis_deadline_schedule(struct task_struct *prev, struct task_struct *next,
			     unsigned int cpu);
void metis_deadline_try_to_wake_up(struct task_struct *task);
void metis_deadline_init(void);
#endif
