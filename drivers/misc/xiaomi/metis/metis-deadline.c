// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: six stock deadline bodies. No state owners or hook wiring. */
#include <linux/cpumask.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/tracepoint.h>
#include "metis_common.h"
#include "metis_deadline.h"

#include "metis-final-trace.h"

static __always_inline void deadline_stock_cpu_bound(unsigned int cpu)
{
	if (cpu >= 32) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
}

static __always_inline struct rb_root_cached *deadline_root(unsigned int cpu)
{
	return &MI_FIELD(per_cpu_ptr(&mi_runqueues, cpu),
			 struct rb_root_cached, 0x1e0);
}

static __always_inline struct rb_node *deadline_node(struct mi_task_struct *state)
{
	return &MI_FIELD(state, struct rb_node, 0x308);
}

struct task_struct *metis_deadline_pick_next_task(unsigned int cpu)
{
	struct mi_task_struct *state;
	struct rb_node *node;
	u64 now;

	deadline_stock_cpu_bound(cpu);
	if (!deadline_enable)
		return NULL;
	node = deadline_root(cpu)->rb_leftmost;
	if (!node)
		return NULL;
	now = fas_sched_clock();
	if (now < MI_FIELD(node, u64, 0x18))
		return NULL;
	state = (void *)((unsigned long)node - 0x308);
	/* Stock emits before checking the node's computed container for NULL. */
	if (trace_metis_deadline_pick_task_enabled())
		trace_metis_deadline_pick_task(state, cpu, fas_sched_clock());
	if (!state)
		return NULL;
	return state->task;
}

void metis_deadline_enqueue_task(unsigned int cpu, struct task_struct *task)
{
	struct mi_task_struct *state;
	struct rb_root_cached *root;
	struct rb_node **link, *parent = NULL, *node;
	u8 *info;
	u32 product;
	s32 budget_ms;
	u64 now, deadline;
	bool leftmost = true;

	deadline_stock_cpu_bound(cpu);
	if (!deadline_enable)
		return;
	state = get_mi_task_struct(task);
	if (!state)
		return;
	info = get_important_task_info(state);
	if (!info || MI_FIELD(info, s32, 0x1c) < 1)
		return;
	if ((MI_FIELD(info, u8, 0x18) & BIT(4)) &&
	    !is_dynamic_vip(state->task->group_leader))
		return;
	if (MI_FIELD(state, u8, 0x300))
		return;
	root = deadline_root(cpu);
	if (!MI_FIELD(state, u64, 0x328)) {
		now = fas_sched_clock();
		/* MUL W followed by SXTW: retain low-32-bit wrap before widening. */
		product = (u32)MI_FIELD(info, s32, 0x1c) * 1000000U;
		MI_FIELD(state, u64, 0x320) = now + (u64)(s64)(s32)product;
		budget_ms = MI_FIELD(info, s32, 0x20);
		product = budget_ms ? (u32)budget_ms * 1000000U : 1000000U;
		MI_FIELD(state, u64, 0x328) = (u64)(s64)(s32)product;
	}
	MI_FIELD(state, u8, 0x300) = 1;
	node = deadline_node(state);
	deadline = MI_FIELD(state, u64, 0x320);
	link = &root->rb_root.rb_node;
	while (*link) {
		parent = *link;
		if (deadline < MI_FIELD(parent, u64, 0x18)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(node, parent, link);
	if (leftmost)
		root->rb_leftmost = node;
	rb_insert_color(node, &root->rb_root);
	if (trace_metis_deadline_enqueue_task_enabled())
		trace_metis_deadline_enqueue_task(state, cpu, fas_sched_clock());
}

void metis_deadline_dequeue_task(unsigned int cpu, struct task_struct *task)
{
	struct mi_task_struct *state;
	struct rb_root_cached *root;
	struct rb_node *node;

	deadline_stock_cpu_bound(cpu);
	if (!task || !deadline_enable)
		return;
	state = get_mi_task_struct(task);
	if (!state)
		state = get_mi_task_struct_unsafe(task);
	if (!state || !MI_FIELD(state, u8, 0x300))
		return;
	root = deadline_root(cpu);
	node = deadline_node(state);
	if (root->rb_leftmost == node)
		root->rb_leftmost = rb_next(node);
	rb_erase(node, &root->rb_root);
	MI_FIELD(state, u8, 0x300) = 0;
	if (trace_metis_deadline_dequeue_task_enabled())
		trace_metis_deadline_dequeue_task(state, cpu, fas_sched_clock());
}

void metis_deadline_schedule(struct task_struct *prev, struct task_struct *next,
			     unsigned int cpu)
{
	struct mi_task_struct *state;
	u8 *rq;
	u64 now, elapsed, budget;

	deadline_stock_cpu_bound(cpu);
	now = fas_sched_clock();
	if (!prev || !deadline_enable)
		return;
	state = get_mi_task_struct(prev);
	if (!state)
		return;
	rq = (u8 *)per_cpu_ptr(&metis_runqueues, cpu);
	elapsed = now - MI_FIELD(rq, u64, 0x278);
	MI_FIELD(rq, u64, 0x278) = now;
	budget = MI_FIELD(state, u64, 0x328);
	MI_FIELD(state, u64, 0x328) = elapsed <= budget ? budget - elapsed : 0;
}

void metis_deadline_try_to_wake_up(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!task || !deadline_enable)
		return;
	state = get_mi_task_struct(task);
	if (state)
		MI_FIELD(state, u64, 0x328) = 0;
}

void metis_deadline_init(void)
{
	u32 possible = (u32)cpumask_bits(cpu_possible_mask)[0];
	unsigned int cpu;

	/* Original snapshots only the low 32 possible bits; no timestamp reset. */
	for (cpu = 0; cpu < 32; cpu++) {
		if (possible & BIT(cpu)) {
			struct rb_root_cached *root = deadline_root(cpu);

			root->rb_root.rb_node = NULL;
			root->rb_leftmost = NULL;
		}
	}
}
