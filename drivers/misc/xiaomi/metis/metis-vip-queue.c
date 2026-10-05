// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: fifteen stock VIP queue/consumer bodies. No queue state owners,
 * module lifecycle, exports, hook registration or build/package wiring. */
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/tracepoint.h>
#include <sched/sched.h>
#include "metis_common.h"
#include "metis_coldstart.h"
#include "metis_link.h"
#include "metis_vip_queue.h"
#include "metis-lowlt-trace.h"

/* Actual stock emission contracts. These declarations preserve the calls
 * and their static-key/current-CPU/preempt gates. Full event definitions,
 * backing tracepoint/static-call owners and metadata remain missing. */
DECLARE_TRACE(metis_vip_task_flag,
	TP_PROTO(struct mi_task_struct *state, unsigned long now),
	TP_ARGS(state, now));
DECLARE_TRACE(metis_enqueue_vip_task,
	TP_PROTO(struct task_struct *task), TP_ARGS(task));
DECLARE_TRACE(mi_pick_next_vip_task,
	TP_PROTO(struct task_struct *task, const char *type), TP_ARGS(task, type));

extern void set_next_entity(struct cfs_rq *cfs, struct sched_entity *entity);

static __always_inline void vip_stock_cpu_bound(unsigned int cpu)
{
	if (cpu >= 32) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
}

static __always_inline struct rb_root_cached *vip_root(struct mi_rq *rq)
{
	return &MI_FIELD(rq, struct rb_root_cached, 0x160);
}

static __always_inline struct list_head *vip_node(struct mi_task_struct *state)
{
	return &MI_FIELD(state, struct list_head, 0x2a0);
}

/* .text+0xea48..0xeb84: disabled priority scheduling resumes after one
 * billion clock units. The subtraction/threshold comparison is unsigned. */
static void prio_sched_check(struct rq *rq)
{
	unsigned int cpu = cpu_of(rq);
	struct metis_rq *state;
	u64 disabled_since;

	vip_stock_cpu_bound(cpu);
	state = per_cpu_ptr(&metis_runqueues, cpu);
	if (MI_FIELD(state, u8, 0x268))
		return;
	disabled_since = MI_FIELD(state, u64, 0x270);
	if (doublecyc_debug)
		pr_info("metis-fas: %s: cpu %d prio sched is disable\n",
			"prio_sched_check", cpu);
	if (!disabled_since)
		return;
	if (doublecyc_debug)
		pr_info("metis-fas: cpu : %d now: %llu, sus_time:%llu, %d, %s\n",
			cpu, fas_sched_clock(), disabled_since,
			current->pid, current->comm);
	if (fas_sched_clock() - disabled_since < 1000000000ULL)
		return;
	if (doublecyc_debug)
		pr_info("metis-fas: cpu : %d prio sched is starting\n", cpu);
	MI_FIELD(state, u64, 0x270) = 0;
	MI_FIELD(state, u8, 0x268) = 1;
}

void __enqueue_vip_task(unsigned int cpu, struct task_struct *task)
{
	struct mi_task_struct *state, *other;
	struct mi_rq *rq;
	struct rb_root_cached *root;
	struct rb_node **link, *parent = NULL;
	struct task_struct *other_task;
	bool leftmost = true, left;

	vip_stock_cpu_bound(cpu);
	if (!task)
		return;
	state = get_mi_task_struct(task);
	if (!state)
		return;
	if (more_vip_flag_print)
		trace_metis_vip_task_flag(state, jiffies);
	MI_FIELD(state, u64, 0x280) = fas_sched_clock();
	if (!cold_start_ui_task(task) && !mi_vip_task(task) &&
	    !mi_link_vip_task(task)) {
		lowlt_list_add_task(task, cpu);
		return;
	}
	if (state->rb_magic_270 == 0xbadc)
		return;
	rq = per_cpu_ptr(&mi_runqueues, cpu);
	root = vip_root(rq);
	clear_task_low_lt(state);
	link = &root->rb_root.rb_node;
	while (*link) {
		parent = *link;
		other = rb_entry(parent, struct mi_task_struct, rb_node_258);
		/* .text+0xed00 self-spins for a fixed NULL container. ed04 loads
		 * task and ed08 returns to ed00 on NULL, so the non-NULL-container
		 * path DOES reload its task every round and can leave that loop
		 * if a concurrent writer supplies a task. No tree repair is added. */
		while (!other)
			barrier();
		do {
			other_task = READ_ONCE(other->task);
		} while (!other_task);
		/* Stock aborts for task == -offsetof(task_struct, se). */
		if ((unsigned long)other_task ==
		    -(unsigned long)offsetof(struct task_struct, se))
			return;
		/* No rank comparison: order is the signed wrapping difference
		 * of CFS vruntime, with equal keys taking the right branch. */
		left = (s64)(task->se.vruntime - other_task->se.vruntime) < 0;
		leftmost &= left;
		link = left ? &parent->rb_left : &parent->rb_right;
	}
	++MI_FIELD(rq, u32, 0x170);
	state->rb_magic_270 = 0xbadc;
	rb_link_node(&state->rb_node_258, parent, link);
	if (leftmost)
		root->rb_leftmost = &state->rb_node_258;
	rb_insert_color(&state->rb_node_258, &root->rb_root);
	list_add_tail(vip_node(state), &MI_FIELD(rq, struct list_head, 0x1b8));
	/* Stock traces/counts even when hardened list insertion reports and
	 * returns without linking the node. It has no failure compensation. */
	trace_mi_enqueue_vip_task(task, "enqueue vip task");
	if (metis_debug)
		pr_info("metis-fas: Task %d comm=%s enqueue vip rbtree, vip_nr_running = %d\n",
			task->pid, task->comm, MI_FIELD(rq, u32, 0x170));
	trace_metis_enqueue_vip_task(task);
}

void __dequeue_vip_task(unsigned int cpu, struct task_struct *task)
{
	struct mi_task_struct *state;
	struct mi_rq *rq;
	struct rb_root_cached *root;
	struct list_head *node;

	vip_stock_cpu_bound(cpu);
	lowlt_list_del_task(task, cpu);
	state = get_mi_task_struct(task);
	if (task && !state) {
		/* Exiting tasks may still own a queued node. Stock reads the
		 * task OEM magic/pointer directly before checking rb_magic. */
		if (task->android_oem_data1[0] != MI_TASK_OEM_MAGIC)
			return;
		state = (void *)task->android_oem_data1[1];
		if (!state || !state->rb_magic_270)
			return;
	}
	if (!state || state->rb_magic_270 != 0xbadc)
		return;
	rq = per_cpu_ptr(&mi_runqueues, cpu);
	root = vip_root(rq);
	if (!RB_EMPTY_NODE(&state->rb_node_258)) {
		if (root->rb_leftmost == &state->rb_node_258)
			root->rb_leftmost = rb_next(&state->rb_node_258);
		rb_erase(&state->rb_node_258, &root->rb_root);
		RB_CLEAR_NODE(&state->rb_node_258);
	}
	node = vip_node(state);
	if (!list_empty(node))
		list_del(node); /* Stock poisons; it does not initialize the node. */
	metis_schlat_dequeue_entity_hook(cpu, task);
	MI_FIELD(state, u8, 0xba) = 0;
	--MI_FIELD(rq, u32, 0x170);
	state->rb_magic_270 = 0;
	if (metis_debug)
		pr_info("metis-fas: task %d dequeue from vip rbtree, vip_nr_running = %d\n",
			task->pid, MI_FIELD(rq, u32, 0x170));
}

void dequeue_vip_task(struct rq *rq, struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	if (state && (state->flags_0xc0 & 0x80))
		state->flags_0xc0 &= 0xffffff5fU;
}

/* Four repeated stock cluster-transfer blocks share the same algorithm.
 * This source helper is not an additional recovered stock function body. */
static void speed_touch_move(struct task_struct *task, unsigned int cpu,
			     unsigned int *last_cpu, s32 *enabled, const char *type)
{
	unsigned int old_cluster = cpu_to_cluster(*last_cpu);
	unsigned int new_cluster = cpu_to_cluster(cpu);

	if (old_cluster != new_cluster && *enabled) {
		restore_boost_cluster(old_cluster);
		boost_cluster(new_cluster, 1, 1, 1);
		if (metis_debug)
			pr_info("metis-fas: speed touch task[%s] %s %d boost cid from %d to %d freq now\n",
				type, task->comm, task->pid, old_cluster, new_cluster);
	}
	*last_cpu = cpu;
}

void before_enqueue_vip_task(struct rq *rq, struct task_struct *task, int flags)
{
	struct mi_task_struct *state = get_mi_task_struct(task), *lookup;
	unsigned int cpu, old_cluster, new_cluster;
	s64 boost_start;
	unsigned long duration;
	bool frame_active;
	s32 uid, previous_cluster;

	if (!state)
		return;
	monitor_task_enqueue(state);
	MI_FIELD(state, u64, 0x280) = fas_sched_clock();
	if (flags & 1) {
		MI_FIELD(state, u32, 0x2b0) = 0;
		MI_FIELD(state, u8, 0xba) = 1;
	}
	if ((state->flags_0xc0 & 0x40) && state->task) {
		lookup = get_mi_task_struct(state->task);
		if (lookup) {
			uid = mi_task_uid(lookup);
			if (foreground_uid && uid == foreground_uid) {
				if (state->flags_0xc0 & 0x40)
					state->flags_0xc0 =
						(state->flags_0xc0 & 0xffffff9fU) | 0x20;
				if (metis_debug)
					pr_info("metis-fas: tsk %d flag is %d\n",
						state->task->pid, state->flags_0xc0);
			}
		}
	}
	metis_wakeup_enqueue_task_hook(rq, task);
	if (metis_xxx_task(task, 0x100UL)) {
		cpu = cpu_of(rq);
		old_cluster = cpu_to_cluster(ui_last_cpu);
		new_cluster = cpu_to_cluster(cpu);
		if (old_cluster != new_cluster) {
			frame_active = false;
			boost_start = frame_boost_start;
			if (boost_start > vtask_monitor_beg_us &&
			    vtask_monitor_beg_us > vtask_monitor_end_us) {
				duration = 0;
				get_frame_boost_end(&duration);
				/* Raw code tests the sign of wrapping arithmetic,
				 * including duration*1000, rather than time_after. */
				frame_active = (s64)((u64)(ktime_to_ns(ktime_get()) / 1000) -
						   (u64)boost_start - duration * 1000UL) < 0;
			}
			if (frame_active) {
				restore_boost_cluster(old_cluster);
				boost_cluster(new_cluster, 1, 1, 1);
			} else if (speed_touch_consumer_in_boost) {
				restore_boost_cluster(old_cluster);
				boost_cluster(new_cluster, 1, 1, 1);
				if (metis_debug)
					pr_info("metis-fas: speed touch task[ui] %s %d boost cid from %d to %d freq now\n",
						task->comm, task->pid, old_cluster, new_cluster);
			}
		}
		ui_last_cpu = cpu;
	}
	if (metis_xxx_task(task, 0x400UL))
		speed_touch_move(task, cpu_of(rq), &ren_last_cpu,
				 &speed_touch_consumer_in_boost, "ren");
	lookup = get_mi_task_struct(task);
	if (lookup && (lookup->flags_0xc0 & 0x800000))
		speed_touch_move(task, cpu_of(rq), &sf_last_cpu,
				 &speed_touch_producer_in_boost, "sf");
	lookup = get_mi_task_struct(task);
	if (lookup && (lookup->flags_0xc0 & 0x1000000))
		speed_touch_move(task, cpu_of(rq), &hwc_last_cpu,
				 &speed_touch_producer_in_boost, "hwc");
	lookup = get_mi_task_struct(task);
	if (lookup && (lookup->flags_0xc0 & 0x200000)) {
		previous_cluster = MI_FIELD(state, s32, 0x298);
		new_cluster = cpu_to_cluster(cpu_of(rq));
		if (metis_debug)
			pr_info("metis-fas: task %d boost cid from %d to %d freq now\n",
				task->pid, previous_cluster, new_cluster);
		/* The old boost is restored even if the cluster has not changed.
		 * Stock then skips reboosting when both clusters are equal. */
		if (previous_cluster != -1)
			restore_boost_cluster(previous_cluster);
		if (previous_cluster != new_cluster) {
			boost_cluster(new_cluster, 1, 1, 1);
			MI_FIELD(state, s32, 0x298) = new_cluster;
		}
	}
	if (metis_debug)
		pr_info("metis-fas: task %d enqueue rbtree vip\n", task->pid);
}

void mi_dequeue_task_fair_hook(struct rq *rq, struct task_struct *task, int flags)
{
	struct mi_task_struct *state;
	unsigned int cluster;

	mi_restore_rt_binder_free(task);
	dequeue_vip_task(rq, task);
	state = get_mi_task_struct(task);
	if (!state)
		return;
	cluster = cpu_to_cluster(cpu_of(rq));
	if (coldstart_app_ui_task(task) || metis_xxx_task(task, 0x100UL)) {
		if (cluster >= 3) {
			asm volatile("brk #0x5512");
			__builtin_unreachable();
		}
		perf_min_freq[cluster] = 0;
	}
}

void register_mi_enqueue_entity_hook(metis_enqueue_entity_fn hook)
{
	mi_enqueue_entity_hooks = hook;
}

void unregister_mi_enqueue_entity_hook(void)
{
	mi_enqueue_entity_hooks = NULL;
}

struct task_struct *mi_pick_task_fair(unsigned int cpu)
{
	struct mi_rq *rq;
	struct metis_rq *state;
	struct rb_node *first;
	struct mi_task_struct *task_state;

	vip_stock_cpu_bound(cpu);
	rq = per_cpu_ptr(&mi_runqueues, cpu);
	first = vip_root(rq)->rb_leftmost;
	state = per_cpu_ptr(&metis_runqueues, cpu);
	/* Stock captures the cached first node BEFORE checking/re-enabling. */
	prio_sched_check(cpu_rq(cpu));
	if (!MI_FIELD(state, u8, 0x268) || !first)
		return NULL;
	task_state = rb_entry(first, struct mi_task_struct, rb_node_258);
	return task_state ? task_state->task : NULL;
}

void mi_pick_next_task_fair_hook(void *unused, struct rq *rq,
		struct task_struct **task, struct sched_entity **entity,
		bool *repick, bool simple, struct task_struct *prev)
{
	unsigned int cpu = cpu_of(rq);
	struct task_struct *picked, *lowlt;
	struct mi_task_struct *state;
	s32 uid;

	picked = metis_deadline_pick_next_task(cpu);
	if (!picked) {
		picked = mi_pick_task_fair(cpu);
		/* Stock task+0x30 is __state, not on_cpu; +0x80 is on_rq. */
		if (prev && prev->on_rq && !prev->__state &&
		    !test_tsk_thread_flag(prev, TIF_NEED_RESCHED)) {
			if (cold_start_ui_task(prev) || cold_start_link_task(prev))
				picked = prev;
			else if (!picked && task_in_cs_protect(prev))
				picked = prev;
		}
	}
	/* Stock calls this even when deadline/VIP already supplied a task. */
	lowlt = lowlt_list_first_task(cpu);
	if (!picked && lowlt && lowlt_tsk_preempt_normal(lowlt))
		picked = lowlt;
	if (*task) {
		state = get_mi_task_struct(*task);
		if (state) {
			uid = mi_task_uid(state);
			if ((uid == 1000 || uid == 0) &&
			    (!picked || !preempt_sysprocess))
				return;
		}
	}
	if (!picked)
		return;
	*task = picked;
	*entity = &picked->se;
	if (simple) {
		while (*entity) {
			set_next_entity(cfs_rq_of(*entity), *entity);
#ifdef CONFIG_FAIR_GROUP_SCHED
			*entity = (*entity)->parent;
#else
			*entity = NULL;
#endif
		}
	}
	*repick = true;
	trace_mi_pick_next_vip_task(picked, "pick vip task");
}

bool mi_preempt_wakeup(struct task_struct *current_task,
		struct task_struct *waking_task, bool *ignore, bool *nopreempt)
{
	u32 current_prio = get_preempt_prio(current_task);
	u32 waking_prio = get_preempt_prio(waking_task);

	if (current_prio != waking_prio) {
		if (current_prio > waking_prio)
			*nopreempt = true;
		*ignore = true;
	}
	/* Equal priority leaves both output bytes unchanged. */
	return current_prio < waking_prio;
}

bool mi_check_preempt_tick(unsigned int cpu)
{
	struct mi_rq *mi_rq;
	struct metis_rq *metis_rq;
	struct rq *rq;
	struct rb_node *first;
	struct mi_task_struct *state;
	struct task_struct *task;
	u32 current_prio, first_prio;

	vip_stock_cpu_bound(cpu);
	mi_rq = per_cpu_ptr(&mi_runqueues, cpu);
	metis_rq = per_cpu_ptr(&metis_runqueues, cpu);
	rq = cpu_rq(cpu);
	first = vip_root(mi_rq)->rb_leftmost;
	prio_sched_check(rq);
	if (!MI_FIELD(metis_rq, u8, 0x268) || !first)
		return false;
	state = rb_entry(first, struct mi_task_struct, rb_node_258);
	if (!state)
		return false;
	task = state->task;
	if (!task)
		return false;
	current_prio = get_preempt_prio(rq->curr);
	state = get_mi_task_struct(task);
	if (!state)
		return false;
	first_prio = get_preempt_prio(task);
	if (first_prio && !current_prio) {
		if (first_prio > 3)
			return true;
		if (rq->clock - MI_FIELD(state, u64, 0x280) > 2000000ULL)
			return true;
	}
	return first_prio && current_prio && first_prio > current_prio;
}

void mi_check_preempt_tick_hook(void *unused, struct cfs_rq *cfs,
				struct sched_entity *entity)
{
	if (cfs->nr_running > 1 && mi_check_preempt_tick(cpu_of(rq_of(cfs))))
		resched_curr(rq_of(cfs));
}

static __always_inline void vip_stock_task_of_warning(struct sched_entity *entity)
{
	if (!entity_is_task(entity) && !(metis_task_of_already_done & 1)) {
		metis_task_of_already_done = 1;
		__warn_printk("!entity_is_task(se)");
		/* Stock WARN's BRK handler can resume at the following branch. */
		asm volatile("brk #0x800");
	}
}

void mi_rb_enqueue_entity_hook(void *unused, struct cfs_rq *cfs,
			       struct sched_entity *entity)
{
	struct task_struct *task;
	metis_enqueue_entity_fn hook;
	unsigned int cpu;

	if (!entity_is_task(entity))
		return;
	task = container_of(entity, struct task_struct, se);
	__enqueue_vip_task(cpu_of(rq_of(cfs)), task);
	cpu = cpu_of(rq_of(cfs));
	vip_stock_task_of_warning(entity);
	metis_deadline_enqueue_task(cpu, task);
	hook = mi_enqueue_entity_hooks;
	if (hook) {
		vip_stock_task_of_warning(entity);
		hook(rq_of(cfs), task);
	}
}

void mi_rb_dequeue_entity_hook(void *unused, struct cfs_rq *cfs,
			       struct sched_entity *entity)
{
	struct task_struct *task;
	unsigned int cpu;

	if (!entity_is_task(entity))
		return;
	task = container_of(entity, struct task_struct, se);
	__dequeue_vip_task(cpu_of(rq_of(cfs)), task);
	cpu = cpu_of(rq_of(cfs));
	vip_stock_task_of_warning(entity);
	metis_deadline_dequeue_task(cpu, task);
}
