// SPDX-License-Identifier: GPL-2.0
/* Functional vendor-hook bodies recovered from stock xr-qi.ko. */
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/clock.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/percpu-rwsem.h>
#include <linux/futex.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <linux/wait.h>
#include "drivers/android/binder_internal.h"
#include "kernel/locking/mutex.h"
#include <trace/hooks/binder.h>
#include <trace/hooks/futex.h>
#include <trace/hooks/dtask.h>
#include <trace/hooks/rwsem.h>
#include <trace/hooks/sched.h>
#include "qi_provider.h"
#include "qi-trace.h"

extern unsigned int sysctl_mutex_qos_opt_spin_time_ns;
extern unsigned int sysctl_mutex_normal_opt_spin_time_ns;
extern struct percpu_rw_semaphore cgroup_threadgroup_rwsem;
extern void qi_free_task_extension(struct task_struct *p);
/* The native rwsem waiter is private to rwsem.c; its recovered contract matches it. */
struct rwsem_waiter {
	struct list_head list;
	struct task_struct *task;
	enum { QI_RWSEM_WRITE, QI_RWSEM_READ } type;
	unsigned long timeout;
	bool handoff_set;
};

static int qi_inherit_traced(struct task_struct *from, struct task_struct *to,
			     void *cookie, u32 kind)
{
	struct task_qi *qi = get_task_qi(to);
	int ret = do_qos_inherit(from, to, cookie, kind);
	{
		switch (kind) {
		case 1: trace_qi_mutex_inherit(ret, from, to, (unsigned long)cookie,
					qi->static_attrs, qi->dynamic_attrs); break;
		case 2: trace_qi_rwsem_inherit(ret, from, to, (unsigned long)cookie,
					qi->static_attrs, qi->dynamic_attrs); break;
		case 3: trace_qi_percpu_rwsem_inherit(ret, from, to, (unsigned long)cookie,
					qi->static_attrs, qi->dynamic_attrs); break;
		case 4: trace_qi_futex_inherit(ret, from, to, (unsigned long)cookie,
					qi->static_attrs, qi->dynamic_attrs); break;
		}
	}
	return ret;
}
static int qi_uninherit_traced(struct task_struct *p, struct task_qi *qi,
			       void *cookie, u32 kind)
{
	int ret = undo_qos_inherit(p, qi, cookie, kind);
	switch (kind) {
	case 1: trace_qi_mutex_uninherit(ret, p, qi->static_attrs, qi->dynamic_attrs,
				       (unsigned long)cookie); break;
	case 2: trace_qi_rwsem_uninherit(ret, p, qi->static_attrs, qi->dynamic_attrs,
				       (unsigned long)cookie); break;
	case 3: trace_qi_percpu_rwsem_uninherit(ret, p, qi->static_attrs, qi->dynamic_attrs,
					      (unsigned long)cookie); break;
	case 4: trace_qi_futex_uninherit(ret, p, (unsigned long)cookie,
				       qi->static_attrs, qi->dynamic_attrs); break;
	}
	return ret;
}

static void qi_free_task(void *unused, struct task_struct *p)
{
	if (p)
		qi_free_task_extension(p);
}
static void qi_select_task_rq_rt(void *unused, struct task_struct *p, int prev_cpu,
			       int sd_flag, int wake_flags, int *new_cpu)
{
	struct task_qi *src, *dst;
	struct qi_event event;
	u32 attrs;
	if (!p || !(wake_flags & 4) || !is_qi_feat_enabled(0) || !is_qos_task(current) ||
	    current->policy == SCHED_NORMAL || current->policy == SCHED_BATCH)
		return;
	src = get_task_qi(current);
	if (!src)
		return;
	attrs = (src->static_attrs & 15) == 3 ? src->static_attrs :
		((src->dynamic_attrs & 15) == 3 ? src->dynamic_attrs : 0);
	if (!attrs)
		return;
	dst = get_alloc_task_qi(p);
	if (!dst || !(attrs & 0x200))
		return;
	event = (struct qi_event){ current, p, attrs, attrs };
	if (!call_qievent_notifiers(6, &event)) {
		spin_lock(&dst->lock);
		dst->dynamic_attrs = event.schedattrs;
		spin_unlock(&dst->lock);
		trace_qi_wake_new_rt_task(current->pid, p->pid, dst->static_attrs, dst->dynamic_attrs);
	}
}

/* Stock transaction +0xb8 is the low word of android_oem_data1. */
static u32 *qi_transaction_pid(struct binder_transaction *t)
{
	return (u32 *)&t->android_oem_data1;
}
static void qi_binder_transaction_init(void *unused, struct binder_transaction *t)
{
	if (is_qi_feat_enabled(1))
		*qi_transaction_pid(t) = 0;
}
static void qi_binder_attach(struct task_struct *from, struct task_struct *to,
			     struct binder_transaction *t, bool proc_context)
{
	struct task_qi *qi;
	int ret;
	struct qi_event event = { NULL, to, 0, 0 };
	get_task_struct(from);
	get_task_struct(to);
	qi = get_alloc_task_qi(to);
	if (is_qos_task(from) && qi) {
		if (!is_qos_task(to)) {
			ret = do_qos_inherit(from, to, t, 0);
			if (proc_context)
				trace_qi_binder_proc_transaction(ret, from, to,
					qi->static_attrs, qi->dynamic_attrs);
			else
				trace_qi_binder_set_priority(ret, t, from, to,
					qi->static_attrs, qi->dynamic_attrs);
			if (ret >= 0) {
				*qi_transaction_pid(t) = to->pid;
				if (!proc_context)
					trace_qi_set_binder_transaction_pid(t, to->pid, to->pid,
									    "set_priority");
			}
		} else {
			call_qievent_notifiers(5, &event);
		}
	}
	put_task_struct(to);
	put_task_struct(from);
}
static void qi_binder_set_priority(void *unused, struct binder_transaction *t,
				   struct task_struct *to)
{
	if (!is_qi_feat_enabled(1) || !t || !to || !t->from || !t->from->task ||
	    (t->flags & TF_ONE_WAY))
		return;
	qi_binder_attach(t->from->task, to, t, false);
}
static void qi_binder_proc_transaction(void *unused, struct task_struct *caller,
		struct task_struct *proc, struct task_struct *thread, int node_debug_id,
		struct binder_transaction *t, bool pending_async)
{
	if (!is_qi_feat_enabled(1) || !caller || !proc || !t ||
	    strcmp(proc->comm, "servicemanager") || !t->need_reply)
		return;
	qi_binder_attach(caller, proc, t, true);
}
static void qi_binder_restore_priority(void *unused, struct binder_transaction *t,
				       struct task_struct *p)
{
	struct task_qi *qi;
	int ret;
	if (!is_qi_feat_enabled(1))
		return;
	get_task_struct(p);
	qi = get_task_qi(p);
	if (qi && (qi->inherit_mask & 1) && t) {
		if (p->pid == *qi_transaction_pid(t)) {
			ret = undo_qos_inherit(p, qi, t, 0);
			if (!ret) {
				trace_qi_restore_binder_transaction_pid(t, *qi_transaction_pid(t),
								      p->pid, "restore_succceed");
				*qi_transaction_pid(t) = ~0U;
			}
			trace_qi_binder_restore_priority(ret, t, p, qi->static_attrs, qi->dynamic_attrs);
		} else {
			trace_qi_restore_binder_transaction_pid(t, *qi_transaction_pid(t), p->pid,
							      "restore_failed");
		}
	}
	put_task_struct(p);
}

static void qi_do_futex(void *unused, int cmd, unsigned int *flags, u32 __user *uaddr)
{
	struct task_qi *src;
	struct task_struct *to;
	u32 word;
	int ret;
	if (!is_qi_feat_enabled(8) || !uaddr || (cmd != 0 && cmd != 9))
		return;
	src = get_task_qi(current);
	if (!src || !is_qos_task(current))
		return;
	pagefault_disable();
	ret = get_user(word, uaddr);
	pagefault_enable();
	if (ret || word <= 65535)
		return;
	rcu_read_lock();
	to = find_task_by_vpid(word >> 16);
	if (IS_ERR_OR_NULL(to) || !get_alloc_task_qi(to)) {
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();
	/* No target reference across this RCU unlock in stock. */
	if (qi_inherit_traced(current, to, (void *)(unsigned long)(word & 65535), 4) >= 0)
		src->futex_word = word;
}
static void qi_futex_wait_end(void *unused, unsigned int flags, u32 bitset)
{
	struct task_qi *src, *dst;
	struct task_struct *to;
	u32 word;
	if (!is_qi_feat_enabled(8))
		return;
	src = get_task_qi(current);
	if (!src || !src->futex_word)
		return;
	word = src->futex_word;
	rcu_read_lock();
	to = find_task_by_vpid((s32)word >> 16);
	if (IS_ERR_OR_NULL(to)) {
		rcu_read_unlock();
		return;
	}
	dst = get_task_qi(to);
	rcu_read_unlock();
	if (dst && (dst->inherit_mask & BIT(4)) &&
	    !qi_uninherit_traced(to, dst, (void *)(unsigned long)(word & 65535), 4))
		src->futex_word = 0;
}
static void qi_futex_wake_traverse(void *unused, struct plist_head *head, int *target,
				 union futex_key key, u32 bitset)
{
	struct task_qi *qi;
	if (!is_qi_feat_enabled(8))
		return;
	qi = get_task_qi(current);
	*target = qi && (qi->inherit_mask & BIT(4)) ?
		(((u32)key.both.word + key.both.offset) >> 2) & 65535 : -1;
}
static void qi_futex_wake_finish(void *unused, int nr_wake, int target)
{
	struct task_qi *qi;
	if (target == -1 || !is_qi_feat_enabled(8))
		return;
	qi = get_task_qi(current);
	if (qi && (qi->inherit_mask & BIT(4)))
		qi_uninherit_traced(current, qi, (void *)(long)target, 4);
}

/* Stock +0x24 is the upper 32 bits of mutex OEM word 0 in its configuration. */
static atomic_t *qi_mutex_waiters(struct mutex *lock)
{
	return (atomic_t *)((u32 *)lock->android_oem_data1 + 1);
}
static void undo_mutex_qos_inherit(struct mutex *lock)
{
	struct task_qi *qi;
	get_task_struct(current);
	qi = get_task_qi(current);
	if (qi && (qi->inherit_mask & BIT(1)))
		qi_uninherit_traced(current, qi, lock, 1);
	put_task_struct(current);
}
static void qi_mutex_wait_start(void *unused, struct mutex *lock)
{
	struct task_struct *owner;
	if (!is_qi_feat_enabled(3))
		return;
	if (is_qi_feat_enabled(9) && is_qos_task(current)) {
		(*(u8 *)&get_alloc_mqos_task(current)->flags) |= 1;
		atomic_inc(qi_mutex_waiters(lock));
		trace_qos_mutex_wait(true, current->pid, (s64)atomic_read(qi_mutex_waiters(lock)));
	}
	if (IS_ERR_OR_NULL(lock))
		return;
	owner = (void *)(atomic_long_read(&lock->owner) & ~7UL);
	if (!owner)
		return;
	get_task_struct(current);
	if (is_qos_task(current)) {
		get_task_struct(owner);
		if (get_alloc_task_qi(owner))
			qi_inherit_traced(current, owner, lock, 1);
		put_task_struct(owner);
	}
	put_task_struct(current);
}
static void qi_mutex_wait_finish(void *unused, struct mutex *lock)
{
	if (!is_qi_feat_enabled(3) || !is_qos_task(current))
		return;
	if ((*(u8 *)&get_alloc_mqos_task(current)->flags) & 1) {
		BUG_ON(!atomic_read(qi_mutex_waiters(lock)));
		atomic_dec(qi_mutex_waiters(lock));
		(*(u8 *)&get_alloc_mqos_task(current)->flags) &= ~1ULL;
	}
	trace_qos_mutex_wait(false, current->pid, (s64)atomic_read(qi_mutex_waiters(lock)));
}
static void qi_record_mutex(void *unused, struct mutex *lock, unsigned long settime)
{
	if (!settime && is_qos_task(current))
		undo_mutex_qos_inherit(lock);
}
static void qi_mutex_unlock(void *unused, struct mutex *lock)
{
	if (is_qi_feat_enabled(3) && is_qos_task(current))
		undo_mutex_qos_inherit(lock);
}
static void do_mutex_list_jump(struct mutex *lock, struct mutex_waiter *waiter, struct list_head *head,
			       bool *already)
{
	struct mutex_waiter *entry;
	if (!is_qi_feat_enabled(2))
		return;
	list_for_each_entry(entry, head, list) {
		get_task_struct(entry->task);
		if (!is_qos_rt_task(entry->task)) {
			list_add_tail(&waiter->list, &entry->list);
			put_task_struct(entry->task);
			*already = true;
			trace_qi_mutex_jump(waiter->task, (unsigned long)lock);
			return;
		}
		put_task_struct(entry->task);
	}
}
static void qi_alter_mutex_list(void *unused, struct mutex *lock, struct mutex_waiter *waiter,
				struct list_head *head, bool *already)
{
	if (!is_qi_feat_enabled(3) || IS_ERR_OR_NULL(lock) || IS_ERR_OR_NULL(waiter) ||
	    IS_ERR_OR_NULL(head) || IS_ERR_OR_NULL(already) || *already)
		return;
	get_task_struct(waiter->task);
	if (is_qos_task(waiter->task))
		do_mutex_list_jump(lock, waiter, head, already);
	put_task_struct(waiter->task);
}
static void qi_mutex_spin_start(void *unused, struct mutex *lock, bool *timeout, int *cnt)
{
	struct task_qi *qi;
	u64 elapsed;
	bool qos;
	if (!is_qi_feat_enabled(9) || !is_qos_task(current))
		return;
	qi = get_task_qi(current);
	if (!qi)
		return;
	if (!qi->mutex_spin_start) {
		qi->mutex_spin_start = sched_clock();
		return;
	}
	if ((++*cnt & 15) != 0)
		return;
	elapsed = sched_clock() - qi->mutex_spin_start;
	qos = is_qos_task(current);
	if ((!qos && elapsed > sysctl_mutex_normal_opt_spin_time_ns) ||
	    (qos && elapsed > sysctl_mutex_qos_opt_spin_time_ns))
		*timeout = true;
	trace_qos_mutex_opt_spin(current->pid, qos, qi->mutex_spin_start, elapsed);
}
static void qi_mutex_spin_finish(void *unused, struct mutex *lock, bool taken)
{
	struct task_qi *qi;
	if (!is_qi_feat_enabled(9) || !is_qos_task(current))
		return;
	qi = get_task_qi(current);
	if (qi && qi->mutex_spin_start)
		qi->mutex_spin_start = 0;
}
static void qi_mutex_can_spin(void *unused, struct mutex *lock, int *ret)
{
	if (is_qi_feat_enabled(9) && *ret && !is_qos_task(current) && current->prio > 99 &&
	    (unsigned int)(atomic_read(qi_mutex_waiters(lock)) - 1) < 999)
		*ret = 0;
}

static void do_qos_set_handoff(struct rwsem_waiter *waiter, struct rw_semaphore *sem)
{
	long count, old;
	struct task_struct *task = waiter->task;
	struct rwsem_waiter *first;
	if (!is_qi_feat_enabled(9) || !is_qos_task(task) || waiter->type != QI_RWSEM_READ)
		return;
	count = atomic_long_read(&sem->count);
	do {
		if (count & 4)
			return;
		old = atomic_long_cmpxchg(&sem->count, count, count | 4);
		if (old == count)
			break;
		count = old;
	} while (1);
	if (list_empty(&sem->wait_list)) {
		waiter->handoff_set = true;
	} else {
		first = list_first_entry(&sem->wait_list, struct rwsem_waiter, list);
		if (first->type != QI_RWSEM_READ)
			return;
		task = first->task;
		first->handoff_set = true;
	}
	(*(u8 *)&get_alloc_mqos_task(task)->flags) |= 2;
	trace_qos_rwsem_wait(true, task->pid, atomic_long_read(&sem->count));
}
static void do_rwsem_list_jump_and_inherit(struct rwsem_waiter *waiter,
					 struct rw_semaphore *sem, bool *already)
{
	struct rwsem_waiter *entry, *first;
	if (!is_qi_feat_enabled(4) || list_empty(&sem->wait_list))
		return;
	first = list_first_entry(&sem->wait_list, struct rwsem_waiter, list);
	list_for_each_entry(entry, &sem->wait_list, list) {
		get_task_struct(entry->task);
		if (waiter->type == QI_RWSEM_WRITE) {
			if (entry->type == QI_RWSEM_READ || !is_qos_rt_task(entry->task)) {
				list_add_tail(&waiter->list, &entry->list);
				if (entry == first && entry->handoff_set) {
					entry->handoff_set = false;
					waiter->handoff_set = true;
				}
				put_task_struct(entry->task);
				*already = true;
				trace_qi_rwsem_jump(waiter->task, (unsigned long)sem);
				return;
			}
			trace_qi_rwsem_skip(entry->task, entry->type == QI_RWSEM_READ,
					    is_qos_rt_task(entry->task));
		} else {
			if (entry->type == QI_RWSEM_READ) {
				trace_qi_rwsem_skip(entry->task, true, is_qos_rt_task(entry->task));
				put_task_struct(entry->task);
				return;
			}
			if (!is_qos_rt_task(entry->task) && get_alloc_task_qi(entry->task) &&
			    get_task_qi(entry->task))
				qi_inherit_traced(waiter->task, entry->task, sem, 2);
		}
		put_task_struct(entry->task);
	}
}
static void qi_alter_rwsem_list(void *unused, struct rwsem_waiter *waiter,
				struct rw_semaphore *sem, bool *already)
{
	struct task_struct *owner;
	unsigned long word;
	if (!is_qi_feat_enabled(5) || IS_ERR_OR_NULL(sem) || IS_ERR_OR_NULL(waiter) ||
	    IS_ERR_OR_NULL(already) || *already)
		return;
	get_task_struct(waiter->task);
	if (is_qos_task(waiter->task)) {
		do_qos_set_handoff(waiter, sem);
		word = atomic_long_read(&sem->owner);
		owner = (void *)(word & ~3UL);
		if (!(word & 1) && owner) {
			get_task_struct(owner);
			if (!get_alloc_task_qi(owner)) {
				put_task_struct(owner);
				goto out;
			}
			if (get_task_qi(owner))
				qi_inherit_traced(waiter->task, owner, sem, 2);
			put_task_struct(owner);
		}
		do_rwsem_list_jump_and_inherit(waiter, sem, already);
	}
 out:
	put_task_struct(waiter->task);
}
static void qi_rwsem_direct_rsteal(void *unused, struct rw_semaphore *sem, bool *steal)
{
	if (is_qi_feat_enabled(9) && atomic_read(&sem->osq.tail))
		*steal = false;
}
static void qi_record_rwsem(void *unused, struct rw_semaphore *sem, unsigned long settime)
{
	struct task_qi *qi;
	if (settime || !is_qi_feat_enabled(5) || !is_qos_task(current))
		return;
	get_task_struct(current);
	qi = get_task_qi(current);
	if (qi && (qi->inherit_mask & BIT(2)))
		qi_uninherit_traced(current, qi, sem, 2);
	put_task_struct(current);
}
static int rwsem_rspin_on_owner(struct rw_semaphore *sem)
{
	unsigned long first = atomic_long_read(&sem->owner), word = first;
	struct task_struct *owner = (void *)(first & ~3UL);
	int type = first & 1 ? 4 : owner ? 2 : 1;
	if (type == 2) {
		word = atomic_long_read(&sem->owner);
		while ((word & ~3UL) == (unsigned long)owner && !((word ^ first) & 3)) {
			if (need_resched() || !READ_ONCE(owner->on_cpu))
				return 8;
			cpu_relax();
			word = atomic_long_read(&sem->owner);
		}
		type = word & 1 ? 4 : (word & ~3UL) ? 2 : 1;
	}
	return type;
}
static void qi_rwsem_opt_rspin(void *unused, struct rw_semaphore *sem, long *adjustment,
			      bool *taken)
{
	struct task_struct *owner;
	unsigned long word;
	int type;
	bool acquired = false;
	u64 start = 0;
	if (!is_qi_feat_enabled(9) || *taken || !is_qos_task(current) || need_resched())
		return;
	word = atomic_long_read(&sem->owner);
	owner = (void *)(word & ~3UL);
	if (owner && !(word & 1) && !READ_ONCE(owner->on_cpu))
		return;
	atomic_long_sub(0x100, &sem->count);
	if (osq_lock(&sem->osq)) {
		if (trace_qos_rwsem_opt_rspin_enabled())
			start = sched_clock();
		type = rwsem_rspin_on_owner(sem);
		while (type & 7) {
			if (!(atomic_long_read(&sem->count) & 1)) {
				if (!(atomic_long_fetch_add(0x100, &sem->count) & 1)) {
					atomic_long_set(&sem->owner, (unsigned long)current |
						(atomic_long_read(&sem->owner) & 2) | 1);
					acquired = true;
					break;
				}
				atomic_long_sub(0x100, &sem->count);
			}
			if (type != 2 && need_resched())
				break;
			cpu_relax();
			type = rwsem_rspin_on_owner(sem);
		}
		osq_unlock(&sem->osq);
		trace_qos_rwsem_opt_rspin(current->pid, start, type, acquired);
		if (acquired) {
			*adjustment = 0;
			*taken = true;
			return;
		}
	}
	atomic_long_add(0x100, &sem->count);
}
static void qi_rwsem_wait_finish(void *unused, struct rw_semaphore *sem)
{
	long count, old;
	/* Stock allocates and dereferences without a feature or allocation check. */
	if ((*(u8 *)&get_alloc_mqos_task(current)->flags) & 2) {
		(*(u8 *)&get_alloc_mqos_task(current)->flags) &= ~2;
		count = atomic_long_read(&sem->count);
		while (count & 4) {
			old = atomic_long_cmpxchg(&sem->count, count, count & ~4UL);
			if (old == count)
				break;
			count = old;
		}
	}
	trace_qos_rwsem_wait(false, current->pid, atomic_long_read(&sem->count));
}

static void qi_record_pcpu_rwsem(void *unused, struct percpu_rw_semaphore *sem,
				 unsigned long settime)
{
	struct task_qi *qi;
	if (settime || sem != &cgroup_threadgroup_rwsem || !is_qi_feat_enabled(7))
		return;
	qi = get_task_qi(current);
	if (qi && (qi->inherit_mask & BIT(3)))
		qi_uninherit_traced(current, qi, sem, 3);
}
static void qi_percpu_rwsem_wq_add(void *unused, struct percpu_rw_semaphore *sem, bool reader)
{
	wait_queue_entry_t *last, *entry;
	struct task_struct *task;
	if (!is_qi_feat_enabled(7) || IS_ERR_OR_NULL(sem) ||
	    sem != &cgroup_threadgroup_rwsem || !is_qos_task(current) ||
	    !atomic_read(&sem->block))
		return;
	if (is_qi_feat_enabled(6)) {
		last = list_last_entry(&sem->waiters.head, wait_queue_entry_t, entry);
		if (!(last->flags & WQ_FLAG_CUSTOM)) {
			list_for_each_entry(entry, &sem->waiters.head, entry) {
				task = entry->private;
				get_task_struct(task);
				if ((entry->flags & WQ_FLAG_CUSTOM) || !is_qos_rt_task(task)) {
					list_move_tail(&last->entry, &entry->entry);
					trace_qi_percpu_rwsem_jump(current, (unsigned long)sem);
					put_task_struct(task);
					break;
				}
				trace_qi_percpu_rwsem_skip(task, !!(entry->flags & WQ_FLAG_CUSTOM),
						  is_qos_rt_task(task));
				put_task_struct(task);
			}
		} else {
			list_for_each_entry(entry, &sem->waiters.head, entry) {
				task = entry->private;
				get_task_struct(task);
				if (entry->flags & WQ_FLAG_CUSTOM) {
					trace_qi_percpu_rwsem_skip(task, true, is_qos_rt_task(task));
					put_task_struct(task);
					break;
				}
				if (get_alloc_task_qi(task) && get_task_qi(task))
					qi_inherit_traced(current, task, sem, 3);
				put_task_struct(task);
			}
		}
	}
	smp_mb();
	rcu_read_lock();
	task = rcu_dereference(sem->writer.task);
	if (task && get_alloc_task_qi(task) && get_task_qi(task))
		qi_inherit_traced(current, task, sem, 3);
	rcu_read_unlock();
}

int qi_vh_comm_register(void)
{
	int ret = register_trace_android_vh_free_task(qi_free_task, NULL);
	if (ret < 0)
		return ret;
	return register_trace_android_rvh_select_task_rq_rt(qi_select_task_rq_rt, NULL);
}
void qi_vh_comm_unregister(void)
{
	/* The restricted RT probe is intentionally left registered by stock. */
	unregister_trace_android_vh_free_task(qi_free_task, NULL);
}
int qi_vh_binder_register(void)
{
	int first = 0, ret;
	ret = register_trace_android_vh_binder_transaction_init(qi_binder_transaction_init, NULL);
	if (ret < 0)
		first = ret;
	ret = register_trace_android_vh_binder_set_priority(qi_binder_set_priority, NULL);
	if (!first && ret < 0)
		first = ret;
	ret = register_trace_android_vh_binder_restore_priority(qi_binder_restore_priority, NULL);
	if (!first && ret < 0)
		first = ret;
	ret = register_trace_android_vh_binder_proc_transaction(qi_binder_proc_transaction, NULL);
	if (!first && ret < 0)
		first = ret;
	if (first < 0)
		pr_err("xr-qi: binder hooks registration %d\n", first);
	return first;
}
void qi_vh_binder_unregister(void)
{
	unregister_trace_android_vh_binder_proc_transaction(qi_binder_proc_transaction, NULL);
	unregister_trace_android_vh_binder_restore_priority(qi_binder_restore_priority, NULL);
	unregister_trace_android_vh_binder_set_priority(qi_binder_set_priority, NULL);
	unregister_trace_android_vh_binder_transaction_init(qi_binder_transaction_init, NULL);
}
#define QI_REGISTER_HOOK(name, fn) do { \
	int _ret = register_trace_##name(fn, NULL); \
	if (_ret < 0) { pr_err("xr-qi: " #name " registration %d\n", _ret); return _ret; } \
} while (0)
int qi_vh_mutex_register(void)
{
	QI_REGISTER_HOOK(android_vh_alter_mutex_list_add, qi_alter_mutex_list);
	QI_REGISTER_HOOK(android_vh_record_mutex_lock_starttime, qi_record_mutex);
	QI_REGISTER_HOOK(android_vh_mutex_unlock_slowpath, qi_mutex_unlock);
	QI_REGISTER_HOOK(android_vh_mutex_wait_start, qi_mutex_wait_start);
	QI_REGISTER_HOOK(android_vh_mutex_wait_finish, qi_mutex_wait_finish);
	QI_REGISTER_HOOK(android_vh_mutex_opt_spin_start, qi_mutex_spin_start);
	QI_REGISTER_HOOK(android_vh_mutex_opt_spin_finish, qi_mutex_spin_finish);
	QI_REGISTER_HOOK(android_vh_mutex_can_spin_on_owner, qi_mutex_can_spin);
	return 0;
}
void qi_vh_mutex_unregister(void)
{
	unregister_trace_android_vh_mutex_wait_start(qi_mutex_wait_start, NULL);
	unregister_trace_android_vh_mutex_wait_finish(qi_mutex_wait_finish, NULL);
	unregister_trace_android_vh_mutex_unlock_slowpath(qi_mutex_unlock, NULL);
	unregister_trace_android_vh_record_mutex_lock_starttime(qi_record_mutex, NULL);
	unregister_trace_android_vh_alter_mutex_list_add(qi_alter_mutex_list, NULL);
	unregister_trace_android_vh_mutex_opt_spin_start(qi_mutex_spin_start, NULL);
	unregister_trace_android_vh_mutex_opt_spin_finish(qi_mutex_spin_finish, NULL);
	unregister_trace_android_vh_mutex_can_spin_on_owner(qi_mutex_can_spin, NULL);
}
int qi_vh_rwsem_register(void)
{
	QI_REGISTER_HOOK(android_vh_alter_rwsem_list_add, qi_alter_rwsem_list);
	QI_REGISTER_HOOK(android_vh_rwsem_direct_rsteal, qi_rwsem_direct_rsteal);
	QI_REGISTER_HOOK(android_vh_record_rwsem_lock_starttime, qi_record_rwsem);
	QI_REGISTER_HOOK(android_vh_rwsem_optimistic_rspin, qi_rwsem_opt_rspin);
	QI_REGISTER_HOOK(android_vh_rwsem_read_wait_finish, qi_rwsem_wait_finish);
	return 0;
}
void qi_vh_rwsem_unregister(void)
{
	unregister_trace_android_vh_alter_rwsem_list_add(qi_alter_rwsem_list, NULL);
	unregister_trace_android_vh_rwsem_direct_rsteal(qi_rwsem_direct_rsteal, NULL);
	unregister_trace_android_vh_record_rwsem_lock_starttime(qi_record_rwsem, NULL);
	unregister_trace_android_vh_rwsem_optimistic_rspin(qi_rwsem_opt_rspin, NULL);
	unregister_trace_android_vh_rwsem_read_wait_finish(qi_rwsem_wait_finish, NULL);
}
int qi_vh_percpu_rwsem_register(void)
{
	QI_REGISTER_HOOK(android_vh_record_pcpu_rwsem_starttime, qi_record_pcpu_rwsem);
	QI_REGISTER_HOOK(android_vh_percpu_rwsem_wq_add, qi_percpu_rwsem_wq_add);
	return 0;
}
void qi_vh_percpu_rwsem_unregister(void)
{
	unregister_trace_android_vh_percpu_rwsem_wq_add(qi_percpu_rwsem_wq_add, NULL);
	unregister_trace_android_vh_record_pcpu_rwsem_starttime(qi_record_pcpu_rwsem, NULL);
}
int qi_vh_futex_register(void)
{
	QI_REGISTER_HOOK(android_vh_do_futex, qi_do_futex);
	QI_REGISTER_HOOK(android_vh_futex_wait_end, qi_futex_wait_end);
	QI_REGISTER_HOOK(android_vh_futex_wake_traverse_plist, qi_futex_wake_traverse);
	QI_REGISTER_HOOK(android_vh_futex_wake_up_q_finish, qi_futex_wake_finish);
	return 0;
}
void qi_vh_futex_unregister(void)
{
	unregister_trace_android_vh_futex_wake_up_q_finish(qi_futex_wake_finish, NULL);
	unregister_trace_android_vh_futex_wake_traverse_plist(qi_futex_wake_traverse, NULL);
	unregister_trace_android_vh_futex_wait_end(qi_futex_wait_end, NULL);
	unregister_trace_android_vh_do_futex(qi_do_futex, NULL);
}
