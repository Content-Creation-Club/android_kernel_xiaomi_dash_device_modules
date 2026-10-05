/* SPDX-License-Identifier: GPL-2.0-only */
/* PARTIAL: four stock low-LT bodies, no state owners, init or exports. */
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include "metis_lowlt.h"
#define CREATE_TRACE_POINTS
#include "metis-lowlt-trace.h"

static __always_inline void lowlt_stock_cpu_bound(unsigned int cpu)
{
	if (cpu >= 32) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
}

void lowlt_list_add_task(struct task_struct *task, unsigned int cpu)
{
	struct mi_task_struct *state = get_mi_task_struct(task);
	struct mi_rq *rq;
	struct list_head *head;
	spinlock_t *lock;
	unsigned long flags;
	u32 count;

	/* The stock provider lookup precedes the fatal CPU bound. */
	lowlt_stock_cpu_bound(cpu);
	if (!state)
		return;
	rq = per_cpu_ptr(&mi_runqueues, cpu);
	if (!rq)
		return;
	if (!(protect_cs && (state->flags_0xc0 & 0x40000)) &&
	    !is_preempted_cs_task(task))
		return;
	lock = &MI_FIELD(rq, spinlock_t, 0x1c8);
	head = &MI_FIELD(rq, struct list_head, 0x1d0);
	spin_lock_irqsave(lock, flags);
	if (list_empty(&state->node_2c0) && state->magic == MI_STATE_MAGIC) {
		list_add_tail(&state->node_2c0, head);
		/* Native trace wrappers retain stock static-key/current-CPU-online
		 * gates and preempt-notrace bracketing under this list lock. */
		trace_mi_enqueue_vip_task(task, "enqueue llt task");
		/* Stock counts even when a returning hardened-list report skipped
		 * the link write. No membership repair or count saturation. */
		count = ++per_cpu(low_lt_num, cpu);
		trace_tracing_mark_write('C', 0, "lowlt list num",
					 (int)cpu, (int)count);
	}
	spin_unlock_irqrestore(lock, flags);
}

void lowlt_list_del_task(struct task_struct *task, unsigned int cpu)
{
	struct mi_task_struct *state = get_mi_task_struct_unsafe(task);
	struct mi_rq *rq;
	spinlock_t *lock;
	unsigned long flags;
	u32 count;

	lowlt_stock_cpu_bound(cpu);
	if (!state)
		return;
	rq = per_cpu_ptr(&mi_runqueues, cpu);
	if (!rq)
		return;
	lock = &MI_FIELD(rq, spinlock_t, 0x1c8);
	spin_lock_irqsave(lock, flags);
	if (!list_empty(&state->node_2c0)) {
		list_del_init(&state->node_2c0);
		count = --per_cpu(low_lt_num, cpu);
		trace_tracing_mark_write('C', 0, "lowlt list num",
					 (int)cpu, (int)count);
	}
	spin_unlock_irqrestore(lock, flags);
	/* Stock clears bit18 outside the lock, including an already empty node. */
	clear_task_low_lt(state);
}

struct task_struct *lowlt_list_first_task(unsigned int cpu)
{
	struct mi_task_struct *state;
	struct mi_rq *rq;
	struct metis_rq *metis_rq;
	struct list_head *head, *node;
	spinlock_t *lock;
	unsigned long flags;

	lowlt_stock_cpu_bound(cpu);
	if (!protect_cs)
		return NULL;
	metis_rq = per_cpu_ptr(&metis_runqueues, cpu);
	if (!MI_FIELD(metis_rq, u8, 0x268))
		return NULL;
	/* Unlike add/delete, stock has no NULL runqueue guard here. */
	rq = per_cpu_ptr(&mi_runqueues, cpu);
	lock = &MI_FIELD(rq, spinlock_t, 0x1c8);
	head = &MI_FIELD(rq, struct list_head, 0x1d0);
	spin_lock_irqsave(lock, flags);
	node = head;
	for (;;) {
		node = node->next;
		if (node == head)
			break;
		state = list_entry(node, struct mi_task_struct, node_2c0);
		/* This checks the node-minus-0x2c0 container, not its stored task.
		 * ERR_PTR(-4096) is accepted; -4095 through -1 are skipped. */
		if (IS_ERR_OR_NULL(state))
			continue;
		spin_unlock_irqrestore(lock, flags);
		/* Preserve stock's unlocked load at state+0xe0, without task ref. */
		return state->task;
	}
	spin_unlock_irqrestore(lock, flags);
	return NULL;
}

bool lowlt_tsk_preempt_normal(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);
	u64 now, elapsed;
	unsigned long begin, end;
	s32 threshold;

	if (!state || !MI_FIELD(state, u64, 0x280))
		return false;
	now = fas_sched_clock();
	/* Stock re-reads the timestamp after the clock call. Its writer and
	 * clock-domain/lifetime contract are still unreconstructed. */
	elapsed = now - MI_FIELD(state, u64, 0x280);
	if (protect_cs && cold_start_app_tgid) {
		end = coldstart_end_jif;
		begin = coldstart_begin_jif;
		/* B.PL/B.MI test the sign of wrapping subtraction, with a literal
		 * 1000-jiffy window, rather than a converted duration. */
		if ((s64)(end - begin) < 0 &&
		    (s64)(jiffies - begin - 1000UL) < 0) {
			threshold = low_lt_preempt_thres_cold_start;
		} else {
			/* Unsynchronized stock reset order; no task/tgid match gate. */
			cold_start_app_tgid = 0;
			home_tgid = 0;
			systemui_tgid = 0;
			systemserver_tgid = 0;
			sf_tgid = 0;
			cold_start_app_mmlock = NULL;
			cold_start_sserver_tgid = 0;
			cold_start_sserver_mmlock = NULL;
			home_mmlock = NULL;
			systemui_mmlock = NULL;
			systemserver_mmlock = NULL;
			sf_mmlock = NULL;
			threshold = low_lt_preempt_thres;
		}
	} else {
		threshold = low_lt_preempt_thres;
	}
	/* LDRSW sign extension followed by CMP/CSET HI: equality is false. */
	return elapsed > (u64)(s64)threshold;
}
