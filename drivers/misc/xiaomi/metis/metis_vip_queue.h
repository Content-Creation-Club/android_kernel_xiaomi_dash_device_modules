/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_VIP_QUEUE_H
#define _XIAOMI_METIS_VIP_QUEUE_H

#include "metis_lowlt.h"
#include "metis_cluster_boost.h"
#include "metis_deadline.h"
#include "metis_monitor.h"
#include "metis_schlat.h"
#include "metis_wakeup.h"

struct rq;
struct cfs_rq;
struct sched_entity;

typedef void (*metis_enqueue_entity_fn)(struct rq *rq,
				      struct task_struct *task);
/* Stock .bss+0x15aa8, indirect-call KCFI family 0x7d2d01f9. */
extern metis_enqueue_entity_fn mi_enqueue_entity_hooks;
extern unsigned int more_vip_flag_print;
extern unsigned int doublecyc_debug;
extern unsigned int ui_last_cpu, ren_last_cpu, sf_last_cpu, hwc_last_cpu;
extern s32 speed_touch_consumer_in_boost, speed_touch_producer_in_boost;
extern s32 preempt_sysprocess;
extern u32 perf_min_freq[3];
/* Loads are 64-bit; the UI migration comparisons are signed. */
extern s64 frame_boost_start, vtask_monitor_beg_us, vtask_monitor_end_us;
/* Stock task_of() warning sites share .data..once+0x0. Its owner and
 * initialization are still absent; do not create per-callsite substitutes. */
extern u8 metis_task_of_already_done;

/* Policy bodies now come from their actual sibling headers. Their full
 * state initialization, registration and provider lifecycle remain absent. */

void __enqueue_vip_task(unsigned int cpu, struct task_struct *task);
void __dequeue_vip_task(unsigned int cpu, struct task_struct *task);
void before_enqueue_vip_task(struct rq *rq, struct task_struct *task, int flags);
void dequeue_vip_task(struct rq *rq, struct task_struct *task);
/* flags is unused by this body, but belongs to its real fair callback ABI. */
void mi_dequeue_task_fair_hook(struct rq *rq, struct task_struct *task, int flags);
void register_mi_enqueue_entity_hook(metis_enqueue_entity_fn hook);
void unregister_mi_enqueue_entity_hook(void);
struct task_struct *mi_pick_task_fair(unsigned int cpu);
void mi_pick_next_task_fair_hook(void *unused, struct rq *rq,
		struct task_struct **task, struct sched_entity **entity,
		bool *repick, bool simple, struct task_struct *prev);
bool mi_preempt_wakeup(struct task_struct *current_task,
		struct task_struct *waking_task, bool *ignore, bool *nopreempt);
bool mi_check_preempt_tick(unsigned int cpu);
void mi_check_preempt_tick_hook(void *unused, struct cfs_rq *cfs,
				struct sched_entity *entity);
void mi_rb_enqueue_entity_hook(void *unused, struct cfs_rq *cfs,
			       struct sched_entity *entity);
void mi_rb_dequeue_entity_hook(void *unused, struct cfs_rq *cfs,
			       struct sched_entity *entity);
#endif
