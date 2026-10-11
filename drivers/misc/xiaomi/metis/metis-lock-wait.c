// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: eleven stock lock-wait producer bodies, no registrations/owners. */
#include <linux/cgroup-defs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/percpu-rwsem.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/tracepoint.h>
/* Canonical current native definition; needs the core kernel include path,
 * as existing metis sources already need for sched/sched.h. No copied layout. */
#include <locking/mutex.h>
#include "../xr_qi/qi_provider.h"
#include "metis_blocked.h"
#include "metis_coldstart.h"
#include "metis_common.h"
#include "metis_lock_wait.h"

#include "metis-final-trace.h"

/* The stock calls the real core corruption reporter even when a list is not
 * inserted. LIST_HARDENED's native declaration carries its calling convention. */
#ifndef CONFIG_LIST_HARDENED
extern bool __list_add_valid_or_report(struct list_head *new,
				       struct list_head *prev, struct list_head *next);
#endif

void mi_mutex_list_add_hook(struct mutex *lock, struct mutex_waiter *waiter,
			    struct list_head *list, bool *already_on_list)
{
	struct list_head *cursor, *next, *prev, *new;
	struct mutex_waiter *other;
	struct task_struct *task;
	bool vip, link_vip;

	/* Original dereferences this result pointer before all other guards. */
	if (READ_ONCE(*already_on_list))
		return;
	if ((is_qi_feat_enabled(3) & 1) && is_qos_task(current))
		return;
	/* Original accesses only the low byte at this callsite. */
	if (READ_ONCE(*(u8 *)&cs_stat_type) & BIT(1))
		protect_pre_cs_task(current);
	if (!lock || !waiter || !list)
		return;
	mutex_reset_data_no_waiter(lock);
	if (!mi_vip_task(current) && !mi_link_vip_task(current))
		return;
	new = &waiter->list;
	cursor = READ_ONCE(list->next);
	for (;;) {
		if (cursor == list) {
			prev = READ_ONCE(list->prev);
			if (new == list || prev == new || READ_ONCE(prev->next) != list) {
				__list_add_valid_or_report(new, prev, list);
			} else {
				WRITE_ONCE(list->prev, new);
				WRITE_ONCE(new->next, list);
				WRITE_ONCE(new->prev, prev);
				WRITE_ONCE(prev->next, new);
			}
			break;
		}
		if (!cursor)
			return;
		other = list_entry(cursor, struct mutex_waiter, list);
		task = READ_ONCE(other->task);
		if (!task)
			return;
		/* Stock captures next before predicates, then reloads other->task for
 		 * the second predicate even when the first result was already true. */
		next = READ_ONCE(cursor->next);
		vip = mi_vip_task(task);
		link_vip = mi_link_vip_task(READ_ONCE(other->task));
		if (!vip && !link_vip) {
			prev = READ_ONCE(cursor->prev);
			next = READ_ONCE(prev->next);
			if (READ_ONCE(next->prev) != prev || prev == new || next == new) {
				__list_add_valid_or_report(new, prev, next);
			} else {
				WRITE_ONCE(next->prev, new);
				WRITE_ONCE(new->next, next);
				WRITE_ONCE(new->prev, prev);
				WRITE_ONCE(prev->next, new);
			}
			break;
		}
		cursor = next;
	}
	/* This is set even when corruption reporting prevented the insertion. */
	WRITE_ONCE(*already_on_list, true);
}

void mi_mutex_wait_start_hook(void *unused, struct mutex *lock)
{
	struct task_struct *task, *owner;
	bool vip, owner_vip;
	int owner_pid, owner_prio;

	if ((is_qi_feat_enabled(3) & 1) && is_qos_task(current))
		return;
	task = current;
	record_mutex_blocked_info(task, lock);
	if (mi_vip_task(task) || mi_link_vip_task(task)) {
		owner = lock ? (void *)((unsigned long)atomic_long_read(&lock->owner) & ~7UL) : NULL;
		if (do_set_lock_vip(owner, task, "mutex") & 1)
			trace_mi_set_mutex_vip_task(task, owner);
	}
	/* Ghidra omitted this entire independent static-key branch. */
	if (!trace_mi_lock_info_enabled())
		return;
	vip = mi_vip_task(task);
	if (!vip)
		vip = mi_link_vip_task(task);
	if (!lock)
		return;
	owner = (void *)((unsigned long)atomic_long_read(&lock->owner) & ~7UL);
	if (!owner || !refcount_read(&owner->usage))
		return;
	/* Preserve the separate usage test followed by unconditional increment. */
	get_task_struct(owner);
	owner_vip = mi_vip_task(owner);
	if (!owner_vip)
		owner_vip = mi_link_vip_task(owner);
	owner_pid = owner->pid;
	owner_prio = owner->prio;
	put_task_struct(owner);
	trace_mi_lock_info(task, vip, "mutex", owner_pid, owner_prio, owner_vip);
}

void mi_rwsem_write_wait_start_hook(void *unused, struct rw_semaphore *sem)
{
	struct task_struct *task = current;
	u64 word;

	if (is_qos_task(task))
		return;
	record_rwsem_blocked_info(task, sem, false);
	if (!sem || (!mi_vip_task(task) && !mi_link_vip_task(task)))
		return;
	if (!((unsigned long)atomic_long_read(&sem->count) & 1))
		return;
	word = READ_ONCE(sem->android_oem_data1[0]);
	if ((word & 0xffffffff00010000ULL) != 0xab9f19ab00010000ULL)
		return;
	do_set_lock_vip((void *)((unsigned long)atomic_long_read(&sem->owner) & ~3UL),
			task, "rwsem");
}

void mi_rwsem_read_wait_start_hook(void *unused, struct rw_semaphore *sem)
{
	struct task_struct *task = current;
	u64 word;

	if (is_qos_task(task))
		return;
	record_rwsem_blocked_info(task, sem, true);
	if (!sem || (!mi_vip_task(task) && !mi_link_vip_task(task)))
		return;
	if (!((unsigned long)atomic_long_read(&sem->count) & 1))
		return;
	word = READ_ONCE(sem->android_oem_data1[0]);
	if ((word & 0xffffffff00010000ULL) != 0xab9f19ab00010000ULL)
		return;
	do_set_lock_vip((void *)((unsigned long)atomic_long_read(&sem->owner) & ~3UL),
			task, "rwsem");
}

void mi_rwsem_lock_starttime(void *unused, struct rw_semaphore *sem,
			   unsigned long start)
{
	u32 stat;
	bool qos;
	struct task_struct *task;

	if (!protect_cs)
		return;
	if (is_qi_feat_enabled(5) & 1) {
		qos = is_qos_task(current);
		if (start && qos)
			return;
	}
	stat = READ_ONCE(cs_stat_type);
	if (!(stat & BIT(0)))
		return;
	if (link_debug > 1)
		pr_info("metis-link: record rwsem lock, cs_stat_type: %d\n", (s32)stat);
	if (sem && (READ_ONCE(cold_start_app_mmlock) == sem ||
		    READ_ONCE(cold_start_sserver_mmlock) == sem ||
		    is_mmaplk_vip_task(current))) {
		task = current;
		if (start)
			coldstart_mark_mmlock_holder(task);
		else
			coldstart_unmark_mmlock_holder(task);
		clean_mmaplk_vip_task(task);
		return;
	}
	mi_record_lock_starttime(current, start, "rwsem");
}

void mi_mutex_lock_starttime(void *unused, struct mutex *lock, unsigned long start)
{
	struct task_struct *task;
	struct mi_task_struct *state;
	u32 stat, flags;

	if (!protect_cs)
		return;
	if (start && (is_qi_feat_enabled(3) & 1) && is_qos_task(current))
		return;
	stat = READ_ONCE(cs_stat_type);
	if (!(stat & BIT(1)))
		return;
	if (link_debug > 1)
		pr_info("metis-link: record mutex lock, cs_stat_type: %d\n", (s32)stat);
	task = current;
	mi_record_lock_starttime(task, start, "mutex");
	if (!start || !(vip_blocked_on_mutex(lock) & 1))
		return;
	if (task && (state = get_mi_task_struct(task))) {
		bool before = (s64)((u64)jiffies - MI_FIELD(state, u64, 0x290)) < 0;

		flags = MI_FIELD(state, u32, 0xc0);
		if (before) {
			if (flags & 0x2000)
				return;
		} else if (flags & 0x2000) {
			clean_mi_vip_task_common(task, 0x2000UL);
		}
	}
	set_mi_vip_task_common(task, 0x2000UL, 50UL);
	if (link_debug)
		pr_info("metis-link: metis link: set mutex vip, pid = %d, name = %s",
			task->pid, task->comm);
}

void mi_rtmutex_lock_starttime(void *unused, struct rt_mutex *lock,
			     unsigned long start)
{
	u32 stat;

	if (!protect_cs)
		return;
	stat = READ_ONCE(cs_stat_type);
	if (!(stat & BIT(2)))
		return;
	if (link_debug > 1)
		pr_info("metis-link: record rtmutex lock, cs_stat_type: %d\n", (s32)stat);
	mi_record_lock_starttime(current, start, "rtmutex");
}

void mi_pcpu_rwsem_lock_starttime(void *unused, struct percpu_rw_semaphore *sem,
				unsigned long start)
{
	u32 stat;

	if (!protect_cs)
		return;
	stat = READ_ONCE(cs_stat_type);
	if (!(stat & BIT(3)))
		return;
	if (link_debug > 1)
		pr_info("metis-link: record pcpu_rwsem lock, cs_stat_type: %d\n", (s32)stat);
	mi_record_lock_starttime(current, start, "pcpu_rwsem");
}

void mi_pcpu_rwsem_wq_add(void *unused, struct percpu_rw_semaphore *sem, bool reader)
{
	u32 stat;

	if ((is_qi_feat_enabled(7) & 1) && is_qos_task(current))
		return;
	stat = READ_ONCE(cs_stat_type);
	if (!(stat & BIT(3)))
		return;
	if (link_debug > 1)
		pr_info("metis-link: pcpu_rwsem lock wq add, cs_stat_type: %d\n", (s32)stat);
	if (sem == &cgroup_threadgroup_rwsem)
		mark_task_low_lt(get_mi_task_struct(current));
}

void mi_rwsem_write_wait_finish_hook(void *unused, struct rw_semaphore *sem)
{
	struct task_struct *task = current;

	clear_rwsem_blocked_info(task, sem);
	if (is_important_mmlock(sem))
		coldstart_mark_mmlock_holder(task);
}

void mi_lock_rwsem_wake_finish_hook(void *unused, struct rw_semaphore *sem)
{
	struct task_struct *task = current;

	mi_lock_vip_finish_common(task);
	if (is_important_mmlock(sem))
		coldstart_unmark_mmlock_holder(task);
}
