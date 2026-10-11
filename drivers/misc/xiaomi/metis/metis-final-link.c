// SPDX-License-Identifier: GPL-2.0-only
/* Stock OS3.0.305 link algorithms. Native build/API/CFI/layout/runtime UNTESTED.
 * Only Xiaomi OEM payload offsets are retained; task/binder/futex use the
 * current canonical kernel definitions. Native rwsem_waiter is shared through
 * root's explicitly authorized internal kernel/locking/rwsem.h header move. */
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/plist.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/uaccess.h>
#include <drivers/android/binder_internal.h>
#include <futex/futex.h>
#include <kernel/locking/rwsem.h>
#include <trace/hooks/binder.h>
#include <trace/hooks/dtask.h>
#include <trace/hooks/futex.h>
#include <trace/hooks/rwsem.h>
#include "../xr_qi/qi_provider.h"
#include "metis_blocked.h"
#include "metis_coldstart.h"
#include "metis_common.h"
#include "metis_extra_controls.h"
#include "metis_wakeup.h"
#include "metis-final-link.h"
#include "metis-final-link-trace.h"

#ifndef CONFIG_LIST_HARDENED
extern bool __list_add_valid_or_report(struct list_head *new,
		struct list_head *prev, struct list_head *next);
#endif

/* Original separate usage test plus unconditional increment is deliberate;
 * replacing it with get_task_struct_rcu() would repair a reference race. */
static void link_put_task(struct task_struct *task)
{
	if (refcount_dec_and_test(&task->usage))
		__put_task_struct(task);
}

/* .text+0xe78, virtual 001098a0. Async sender lookup is dereferenced after
 * its RCU read section and before taking a reference, as in stock. */
void do_set_binder_vip(struct binder_transaction *transaction,
		struct task_struct *target)
{
	struct task_struct *sender;
	struct mi_task_struct *state, *reload;
	unsigned int flags;
	bool vip = false, async_vip = false;

	if (!target)
		return;
	flags = transaction->flags;
	if (!refcount_read(&target->usage))
		return;
	get_task_struct(target);
	if (flags & TF_ONE_WAY) {
		if (!transaction->from_pid)
			goto put_target;
		rcu_read_lock();
		sender = find_task_by_vpid(transaction->from_pid);
		rcu_read_unlock();
	} else {
		if (!transaction->from)
			goto put_target;
		sender = transaction->from->task;
	}
	if (!sender || !(state = get_mi_task_struct(sender)))
		goto put_target;
	get_task_struct(sender);
	if (flags & TF_ONE_WAY) {
		reload = get_mi_task_struct(sender);
		if (reload) {
			if ((s64)((u64)jiffies - MI_FIELD(reload, u64, 0x290)) < 0)
				async_vip = !!(reload->flags_0xc0 & 0x10000);
			else if (reload->flags_0xc0 & 0x10000)
				clean_mi_vip_task_common(sender, 0x10000);
		}
	} else {
		vip = mi_vip_task(sender);
		if (!vip)
			vip = mi_link_vip_task(sender);
	}
	if (vip || async_vip) {
		if (vip)
			MI_FIELD(state, s32, 0x2b0) = target->pid;
		set_mi_vip_task_common(target, 0x4000, async_vip ? 12 : 50);
		if (link_debug)
			pr_info("metis-link: metis link: %s binder vtsk %d src task, pid = %d, name = %s\n",
				flags & TF_ONE_WAY ? "ASYNC" : "SYNC",
				target->pid, sender->pid, sender->comm);
		/* .text+0x1160..0x11cc, omitted by the decompiler. */
		trace_mi_set_binder_vip_task(sender, target, 0x4000);
	}
	link_put_task(sender);
put_target:
	link_put_task(target);
}

/* .text+0x35b8, virtual 0010bfe0. W0 has boolean low-bit semantics; original
 * C return spelling is UNKNOWN, retained as the existing u32 contract. */
u32 do_set_lock_vip(struct task_struct *owner, struct task_struct *waiter,
		const char *type)
{
	struct mi_task_struct *state;
	u32 result = 0;

	if (!owner || !waiter || !refcount_read(&owner->usage))
		return 0;
	get_task_struct(owner);
	state = get_mi_task_struct(owner);
	if (state) {
		if ((s64)((u64)jiffies - state->deadline_0x290) < 0) {
			if (state->flags_0xc0 & 0x2000)
				goto put_owner;
		} else if (state->flags_0xc0 & 0x2000) {
			clean_mi_vip_task_common(owner, 0x2000);
		}
	}
	set_mi_vip_task_common(owner, 0x2000, 50);
	if (link_debug)
		pr_info("metis-link: metis link: %s vtsk %d src task, pid = %d, name = %s\n",
			type, owner->pid, waiter->pid, waiter->comm);
	result = 1;
put_owner:
	link_put_task(owner);
	return result;
}

static bool binder_important_process(const char *name)
{
	return !strncmp(name, "system_server", TASK_COMM_LEN) ||
		!strncmp(name, "com.miui.home", TASK_COMM_LEN) ||
		!strncmp(name, "surfaceflinger", TASK_COMM_LEN);
}

/* .text+0x13fb0, virtual 0011c9d8. Native hook/binder_internal.h prove all
 * unused callback formals and replace original free_async_space-0x1a0. */
void android_vh_binder_alloc_new_buf_locked(void *data, size_t size,
		size_t *free_async_space, int is_async, bool *should_fail)
{
	struct binder_alloc *alloc;
	struct binder_proc *proc;
	struct task_struct *other;
	struct mi_task_struct *state;
	s32 ticks = (s32)((u32)binder_boost_msec * 250U);

	if ((u32)ticks + 999U < 1999U)
		return;
	alloc = container_of(free_async_space, struct binder_alloc, free_async_space);
	proc = container_of(alloc, struct binder_proc, alloc);
	other = proc->tsk;
	/* Stock reads both leaders without NULL/lifetime checks. */
	if (!binder_important_process(current->group_leader->comm) &&
	    !binder_important_process(other->group_leader->comm))
		return;
	state = get_mi_task_struct(current);
	if (!state)
		return;
	/* The decompiler mislabeled X0 as division output. Raw +0x140b8 calls
 * the existing setter with the real mi_task_struct still in X0. */
	set_mmaplk_vip_task(state);
	trace_metis_set_biner_alloc_new_buf_vip(current, other, ticks / 1000,
			MI_FIELD(state, u32, 0x2b4));
}

/* .text+0x1d48, virtual 0010a770; binder.h supplies ignored transaction. */
void mi_binder_restore_vip_hook(void *data, struct binder_transaction *transaction,
		struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!task || !refcount_read(&task->usage))
		return;
	if (task->prio < 100)
		set_mi_vip_task_common_noresched(task, 0x8000, 12);
	get_task_struct(task);
	state = get_mi_task_struct(task);
	if (state && (state->flags_0xc0 & 0x4000)) {
		bool before = (s64)((u64)jiffies - state->deadline_0x290) < 0;

		clean_mi_vip_task_common(task, 0x4000);
		if (before)
			trace_mi_clean_binder_vip_task(current, task, 0x4000);
	}
	link_put_task(task);
}

/* .text+0x1f10, virtual 0010a938. */
void mi_binder_set_vip_hook(void *data, struct binder_transaction *transaction,
		struct task_struct *task)
{
	if (!transaction || !task)
		return;
	if ((is_qi_feat_enabled(1) & 1) &&
	    is_qos_task(transaction->from ? transaction->from->task : NULL))
		return;
	do_set_binder_vip(transaction, task);
}

/* .text+0x2744, virtual 0010b120; native futex/futex.h owns futex_q. */
void mi_alter_futex_plist_add(void *data, struct plist_node *node,
		struct plist_head *head, bool *already_on_hb)
{
	struct list_head *cursor, *next;
	struct plist_node *entry;
	struct futex_q *other;
	struct task_struct *task = current, *waiter;
	int prev_pid = 0;
	bool prev_vip = true;

	if ((is_qi_feat_enabled(8) & 1) && is_qos_task(task))
		return;
	if (*already_on_hb || metis_wakeup_futex_plist_insert(&node->prio))
		return;
	if ((!mi_vip_task(task) && !mi_link_vip_task(task)) ||
	    (u32)(task->policy - 1U) <= 1)
		return;
	cursor = head->node_list.next;
	while (cursor != &head->node_list) {
		next = cursor->next;
		entry = list_entry(cursor, struct plist_node, node_list);
		other = container_of(entry, struct futex_q, list);
		waiter = other->task;
		if ((!other->pi_state || !other->rt_waiter) &&
		    (u32)(waiter->policy - 1U) > 1 &&
		    !mi_vip_task(waiter) && !mi_link_vip_task(waiter)) {
			if (link_debug)
				pr_info("metis-link: futex_list_add: prev_pid=%d, prev_vip=%d, next_pid=%d, next_vip=%d, next_comm=%s, cur pid=%d, cur comm=%s\n",
					prev_pid, prev_vip, other->task->pid,
					mi_vip_task(other->task) || mi_link_vip_task(other->task),
					other->task->comm, task->pid, task->comm);
			list_add(&node->node_list, cursor->prev);
			*already_on_hb = true;
			return;
		}
		prev_pid = other->task->pid;
		prev_vip = mi_vip_task(other->task) || mi_link_vip_task(other->task);
		cursor = next;
	}
	*already_on_hb = false;
}

/* .text+0x28ac, virtual 0010b2d4. Native get_user retains user-access fault
 * handling/pagefault_disable; the raw error W8 branch is absent in Ghidra. */
void mi_do_futex(void *data, int command, unsigned int *flags, u32 __user *uaddr2)
{
	struct task_struct *task = current, *ui, *owner;
	struct mi_task_struct *state;
	u32 value;
	int error, owner_pid;

	if ((is_qi_feat_enabled(8) & 1) && is_qos_task(task))
		return;
	if (link_debug > 3)
		pr_info("metis-link: %s: cmd %d flags %d uaddr2 0x%p, cur %d %s\n",
			__func__, command, *flags, uaddr2, task->pid, task->comm);
	switch (command) {
	case 0:
	case 9:
		if (!mi_vip_task(task) && !mi_link_vip_task(task))
			return;
		state = get_mi_task_struct(task);
		if (!uaddr2 || !state)
			return;
		pagefault_disable();
		error = get_user(value, uaddr2);
		MI_FIELD(state, s32, 0x2b0) = (s32)value;
		pagefault_enable();
		if (error) {
			if (link_debug)
				pr_err("metis-link: mi_do_futex: __get_user err, pid=%d, comm=%s\n",
					task->pid, task->comm);
			return;
		}
		MI_FIELD(state, u32, 0x2b4) |= 2;
		if (is_qi_feat_enabled(8) & 1) {
			MI_FIELD(state, s32, 0x2b0) = MI_FIELD(state, s16, 0x2b2);
			trace_mi_get_futex_owner_pid(task->pid, MI_FIELD(state, s32, 0x2b0));
		}
		if (link_debug)
			pr_info("metis-link: mi_do_futex, set blocked owner %p, pid=%d, comm=%s\n",
				uaddr2, task->pid, task->comm);
		return;
	case 16:
		if (!uaddr2)
			return;
		pagefault_disable();
		error = get_user(value, uaddr2);
		pagefault_enable();
		if (error) {
			pr_err("metis-link: %s: __get_user err, pid=%d, comm=%s\n",
				"juc_notify_task", task->pid, task->comm);
			return;
		}
		if ((s32)value < 0 || (value & 0xffff) != get_coldstart_app_tgid())
			return;
		owner_pid = (s32)value >> 16;
		rcu_read_lock();
		if (coldstart_app_ui_task(task)) {
			ui = task;
			owner = pid_task(find_vpid(owner_pid), PIDTYPE_PID);
		} else {
			ui = pid_task(find_vpid(value & 0xffff), PIDTYPE_PID);
			owner = task;
		}
		if (!ui || !refcount_read(&ui->usage) ||
		    !owner || !refcount_read(&owner->usage)) {
			rcu_read_unlock();
			return;
		}
		get_task_struct(ui);
		get_task_struct(owner);
		rcu_read_unlock();
		if ((mi_vip_task(ui) || mi_link_vip_task(ui)) &&
		    (state = get_mi_task_struct(ui))) {
			if (!coldstart_app_ui_task(task))
				set_mi_vip_task_common(owner, 0x20000, 25);
			MI_FIELD(state, s32, 0x2b0) = owner_pid;
			MI_FIELD(state, u32, 0x2b4) |= 2;
		}
		link_put_task(ui);
		link_put_task(owner);
		return;
	case 17:
		if (coldstart_app_tasks(task))
			set_mi_vip_task_common(task, 0x80000, 50);
		return;
	case 18:
		clean_mi_vip_task_common(task, 0x80000);
	}
}

/* .text+0x2e5c, virtual 0010b884. */
void mi_futex_sleep_start(void *data, struct task_struct *task)
{
	struct mi_task_struct *state, *owner_state;
	struct task_struct *owner;

	if ((is_qi_feat_enabled(8) & 1) && is_qos_task(task))
		return;
	if (coldstart_app_ui_task(task))
		cold_start_ui_futex_wait = jiffies;
	if ((!mi_vip_task(task) && !mi_link_vip_task(task)) ||
	    !(state = get_mi_task_struct(task)) ||
	    !(MI_FIELD(state, u32, 0x2b4) & 2) || MI_FIELD(state, s32, 0x2b0) < 1)
		return;
	rcu_read_lock();
	owner = pid_task(find_vpid(MI_FIELD(state, s32, 0x2b0)), PIDTYPE_PID);
	if (!owner || !refcount_read(&owner->usage)) {
		rcu_read_unlock();
		return;
	}
	get_task_struct(owner);
	rcu_read_unlock();
	if (!get_mi_task_struct(owner))
		goto put_owner;
	owner_state = get_mi_task_struct(owner);
	if (owner_state) {
		if ((s64)((u64)jiffies - owner_state->deadline_0x290) < 0) {
			if (owner_state->flags_0xc0 & 0x20000) {
				if (link_debug)
					pr_warn("metis-link: try to set art-mutex vip task: pid=%d, name=%s for futex_vip task: pid=%d, comm=%s, already is vip\n",
						owner->pid, owner->comm, task->pid, task->comm);
				MI_FIELD(state, u32, 0x2b4) |= 4;
				goto put_owner;
			}
		} else if (owner_state->flags_0xc0 & 0x20000) {
			clean_mi_vip_task_common(owner, 0x20000);
		}
	}
	set_mi_vip_task_common(owner, 0x20000, 25);
	if (link_debug)
		pr_info("metis-link: set art-mutex vip task: pid=%d, name=%s for futex_vip task: pid=%d, comm=%s\n",
			owner->pid, owner->comm, task->pid, task->comm);
	/* Stock DMB ISH follows publication and precedes wake-race cleanup. */
	smp_mb();
	if (MI_FIELD(state, u32, 0x2b4) & 4) {
		clean_mi_vip_task_common(owner, 0x20000);
		if (link_debug)
			pr_info("metis-link: clean art-mutex vip task immediately: pid=%d, name=%sfor futex vip task: pid=%d, comm=%s\n",
				owner->pid, owner->comm, task->pid, task->comm);
	}
	MI_FIELD(state, u32, 0x2b4) |= 4;
put_owner:
	link_put_task(owner);
}

/* .text+0x31a8, virtual 0010bbd0. Native futex hook proves four ignored args. */
void mi_futex_wake_this(void *data, int ret, int nr_wake, int target_nr,
		struct task_struct *task)
{
	struct mi_task_struct *state;

	if ((is_qi_feat_enabled(8) & 1) && is_qos_task(current))
		return;
	state = get_mi_task_struct(task);
	if (!state || !(MI_FIELD(state, u32, 0x2b4) & 6) ||
	    !get_mi_task_struct(current))
		return;
	clean_mi_vip_task_common(current, 0x20000);
	MI_FIELD(state, s32, 0x2b0) = 0;
	MI_FIELD(state, u32, 0x2b4) |= 4;
	if (link_debug)
		pr_info("metis-link: clean futex vtask: %s(%d), blocked tsk: %s(%d)\n",
			current->comm, current->pid, task->comm, task->pid);
}

/* .text+0x3264, virtual 0010bc8c; native callback flags/bitset are unused. */
void mi_futex_wait_end(void *data, unsigned int flags, u32 bitset)
{
	struct mi_task_struct *state;
	struct task_struct *owner;

	if (coldstart_app_ui_task(current)) {
		cold_start_ui_futex_wait = 0;
		last_ui_forked_pid = 0;
	}
	state = get_mi_task_struct(current);
	if (!state)
		return;
	MI_FIELD(state, u32, 0x2b4) &= ~6U;
	if (MI_FIELD(state, s32, 0x2b0) > 0) {
		rcu_read_lock();
		owner = pid_task(find_vpid(MI_FIELD(state, s32, 0x2b0)), PIDTYPE_PID);
		if (owner && refcount_read(&owner->usage)) {
			get_task_struct(owner);
			clean_mi_vip_task_common(owner, 0x20000);
			link_put_task(owner);
		}
		rcu_read_unlock();
	}
	MI_FIELD(state, s32, 0x2b0) = 0;
}

/* .text+0x3744, virtual 0010c16c. No private rwsem waiter access occurs here. */
u32 task_prio_for_rwsem_insert(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!task)
		return 5;
	if (coldstart_app_ui_task(task))
		return 0;
	if ((u32)(task->policy - 3U) > 0xfffffffdU)
		return 1;
	if (mi_vip_task(task))
		return 2;
	if (!mi_link_vip_task(task)) {
		if (coldstart_app_tasks(task) || coldstart_sserver_tasks(task))
			return 4;
		return 5;
	}
	state = get_mi_task_struct(task);
	return state && (state->flags_0xc0 & 0x08080000) ? 3 : 2;
}

/* Exact stock hardening behavior: report corruption without insertion, then
 * the original caller still marks already_on_list true. Helper count zero. */
static void rwsem_link_after(struct list_head *new, struct list_head *prev)
{
	struct list_head *next = READ_ONCE(prev->next);

	if (READ_ONCE(next->prev) != prev || new == prev || new == next) {
		__list_add_valid_or_report(new, prev, next);
		return;
	}
	WRITE_ONCE(next->prev, new);
	WRITE_ONCE(new->next, next);
	WRITE_ONCE(new->prev, prev);
	WRITE_ONCE(prev->next, new);
}

/* .text+0x3868, virtual 0010c290. Canonical waiter comes from the root's
 * authorized kernel/locking/rwsem.h move, also included by native rwsem.c. */
void do_rwsem_list_insert(struct rwsem_waiter *waiter,
		struct rw_semaphore *sem, bool *already_on_list, int priority)
{
	struct list_head *cursor = READ_ONCE(sem->wait_list.prev), *previous;
	struct rwsem_waiter *other;
	struct task_struct *task;
	unsigned long timeout;
	int other_priority;

	if (priority == 5)
		return;
	while (cursor != &sem->wait_list) {
		if (!cursor)
			return;
		other = list_entry(cursor, struct rwsem_waiter, list);
		task = READ_ONCE(other->task);
		if (!task)
			return;
		previous = READ_ONCE(cursor->prev);
		other_priority = -1;
		if (!READ_ONCE(other->handoff_set)) {
			timeout = READ_ONCE(other->timeout);
			if ((s64)(timeout - jiffies + 5UL) < 0) {
				if (link_debug)
					pr_info("metis-link: rwsem tsk %d %s timeout, cur %lu timeout %lu, %ld\n",
						task->pid, task->comm, jiffies, timeout,
						(long)(jiffies - timeout));
			} else {
				other_priority = task_prio_for_rwsem_insert(task);
			}
		}
		if (priority >= other_priority) {
			rwsem_link_after(&waiter->list, cursor);
			*already_on_list = true;
			/* Raw .text+0x3968..0x3998 __trace_bprintk tail, skipped
 * by Ghidra. Preserve the genuine native debug producer. */
			trace_printk("tsched: insert tsk %d %s %d after %d %s %d\n",
				current->pid, current->comm, priority,
				other->task->pid, other->task->comm, other_priority);
			return;
		}
		cursor = previous;
	}
	rwsem_link_after(&waiter->list, &sem->wait_list);
	*already_on_list = true;
}

/* .text+0x19ac, virtual 0010a3d4. Native rwsem waiter enum/list/task fields.
 * MM identity follows actual mm->mmap_lock, never stock task/mm offsets. */
void mi_rwsem_list_add_hook(void *unused, struct rwsem_waiter *waiter,
		struct rw_semaphore *sem, bool *already_on_list)
{
	struct task_struct *task = current;
	u32 priority;
	unsigned long biased, reserved;

	if (*already_on_list || is_qos_task(task))
		return;
	if (READ_ONCE(*(u8 *)&cs_stat_type) & 1)
		protect_pre_cs_task(task);
	if (!waiter || !sem)
		return;
	priority = task_prio_for_rwsem_insert(task);
	if (!protect_cs)
		goto insert;
	if (coldstart_app_tasks(task) && task->mm && &task->mm->mmap_lock == sem)
		cold_start_app_mmlock = sem;
	if (coldstart_sserver_tasks(task) && task->mm && &task->mm->mmap_lock == sem) {
		if (link_debug)
			pr_info("metis-link: system server:can set system server mmaplock\n");
		cold_start_sserver_mmlock = sem;
	}
	if (is_home_tasks(task) && task->mm && &task->mm->mmap_lock == sem)
		home_mmlock = sem;
	if (is_systemui_tasks(task) && task->mm && &task->mm->mmap_lock == sem)
		systemui_mmlock = sem;
	if (is_systemserver_tasks(task) && task->mm && &task->mm->mmap_lock == sem)
		systemserver_mmlock = sem;
	if (is_sf_tasks(task) && task->mm && &task->mm->mmap_lock == sem)
		sf_mmlock = sem;
	if (is_important_mmlock(sem)) {
		if (waiter->type == RWSEM_WAITING_FOR_READ) {
			coldstart_mark_mmlock_holder(task);
		} else if (waiter->type == RWSEM_WAITING_FOR_WRITE) {
			if (priority < 4) {
				if (!priority)
					waiter->timeout = jiffies;
			} else if (READ_ONCE(*(u8 *)&cs_stat_type) & 2) {
				biased = waiter->timeout + (long)(s32)reader_biased_jif;
				reserved = rtg_handoff_min_inter + rtg_rwsem_handoff_jif;
				waiter->timeout = max(biased, reserved);
			}
			rtg_rwsem_handoff_jif = waiter->timeout;
		}
	}
	if (coldstart_app_tasks(task) && !coldstart_set_mmlock_holder_vip)
		mark_task_low_lt(get_mi_task_struct(task));
insert:
	do_rwsem_list_insert(waiter, sem, already_on_list, priority);
}

/* .text+0x2388, virtual 0010adb0. A queue/head timeout is observed under
 * native raw wait_lock; predicates/count selection continue after unlocking.
 * It may dereference a NULL head task in debug timeout reporting, as stock. */
void mi_rwsem_read_trylock_failed(void *unused, struct rw_semaphore *sem,
		long *count, int *ret)
{
	struct rwsem_waiter *head;
	struct task_struct *task;
	enum rwsem_waiter_type type;
	unsigned long timeout;
	u64 mask;
	bool head_vip = false, vip;

	if (!protect_cs || !cold_start_app_tgid ||
	    ((is_qi_feat_enabled(5) & 1) && is_qos_task(current)))
		return;
	raw_spin_lock_irq(&sem->wait_lock);
	if (list_empty(&sem->wait_list))
		goto unlock;
	head = list_first_entry(&sem->wait_list, struct rwsem_waiter, list);
	task = READ_ONCE(head->task);
	if (task && task->prio < 100)
		goto unlock;
	timeout = READ_ONCE(head->timeout);
	if ((s64)(timeout - jiffies + 5UL) < 0) {
		if (link_debug)
			pr_info("metis-link: rwsem tsk %d %s timeout, cur %lu timeout %lu, %ld\n",
				task->pid, task->comm, jiffies, timeout,
				(long)(jiffies - timeout));
		goto unlock;
	}
	type = head->type;
	if ((READ_ONCE(*(u8 *)&protect_cs) & 2) &&
	    (current->tgid == cold_start_sserver_tgid || current->tgid == cold_start_app_tgid) && task)
		head_vip = mi_vip_task(task) || mi_link_vip_task(READ_ONCE(head->task));
	raw_spin_unlock_irq(&sem->wait_lock);
	vip = mi_vip_task(current) || mi_link_vip_task(current);
	if ((current->tgid == cold_start_app_tgid || current->tgid == cold_start_sserver_tgid) &&
	    current->mm && &current->mm->mmap_lock == sem &&
	    (READ_ONCE(*(u8 *)&protect_cs) & 2)) {
		if (!vip || (head_vip && (s64)(jiffies - timeout) >= 0)) {
			if (type == RWSEM_WAITING_FOR_WRITE && head_vip)
				return;
			mask = 0x8000000000000005ULL;
			if (type != RWSEM_WAITING_FOR_WRITE ||
			    (s64)(jiffies - timeout + (long)(s32)read_trylock_reserved_jif) < 0)
				goto apply;
			/* Original computes the elapsed low W delta AND 3, not a
 * duration clamp. Preserve this modulo-four blocked-reader condition. */
			if (!cold_start_ui_futex_wait ||
			    (s64)(((u32)jiffies - (u32)cold_start_ui_futex_wait) & 3) >=
			    (s64)(s32)ui_blocked_reader_jif)
				goto ordinary;
		}
		mask = 0x8000000000000001ULL;
		goto apply;
	}
ordinary:
	mask = vip && !cold_start_app_tgid ?
		0x8000000000000005ULL : 0x8000000000000007ULL;
apply:
	if (!count) {
		/* Stock LDR XZR,[sem] still performs the native count load. */
		(void)atomic_long_read(&sem->count);
		return;
	}
	if (!((u64)*count & mask))
		*ret = 1;
	return;
unlock:
	raw_spin_unlock_irq(&sem->wait_lock);
}

/* .text+0x3a14, virtual 0010c43c; X2 saved for real restore trace emission. */
void mi_record_lock_starttime(struct task_struct *task, unsigned long start,
		const char *type)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	if (!state)
		return;
	if ((is_qi_feat_enabled(5) & 1) && is_qos_task(task) && start)
		return;
	if (start) {
		protect_cs_task(task);
		clear_task_low_lt(state);
	} else if (MI_FIELD(state, u32, 0x2b4) & 8) {
		state->flags_0xc0 &= ~0x2000U;
		clear_preemption_protect(task);
		trace_mi_restore_lock_vip_task(current, task, type);
	}
}

/* .text+0x3b18, virtual 0010c540. */
void mi_lock_vip_finish_common(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!task || !refcount_read(&task->usage))
		return;
	get_task_struct(task);
	state = get_mi_task_struct(task);
	if (state && (state->flags_0xc0 & 0x2000))
		clean_mi_vip_task_common(task, 0x2000);
	link_put_task(task);
}

/* .text+0x2650, virtual 0010b078; native wake-up callback is data/task. */
void metis_link_try_to_wake_up_hook(void *data, struct task_struct *task)
{
	struct mi_task_struct *state, *wake_state;

	metis_wakeup_try_to_wakeup(task);
	mi_lock_vip_finish_common(current);
	if (task && current && (state = get_mi_task_struct(current))) {
		if ((s64)((u64)jiffies - state->deadline_0x290) < 0) {
			if (!(state->flags_0xc0 & 0x4000) ||
			    !(wake_state = get_mi_task_struct(task)) ||
			    MI_FIELD(wake_state, s32, 0x2b0) != current->pid)
				goto ready;
			MI_FIELD(wake_state, s32, 0x2b0) = 0;
		} else if (!(state->flags_0xc0 & 0x4000)) {
			goto ready;
		}
		clean_mi_vip_task_common(current, 0x4000);
	}
ready:
	coldstart_tsk_ready_to_run(task);
}

/* .text+0x1374, virtual 00109d9c. Bounds failure really is BRK #0x5512. */
void register_rvh_mi_try_to_wake_up(unsigned int module, metis_link_wake_fn hook)
{
	if (module >= 3) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	mi_try_to_wake_up_func[module] = hook;
	pr_info("metis-link: %s[%d] now\n", __func__, module);
}

/* .text+0x13c0, virtual 00109de8. No RCU or hook synchronization in stock. */
void unregister_rvh_mi_try_to_wake_up(unsigned int module)
{
	if (module >= 3) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	mi_try_to_wake_up_func[module] = NULL;
	pr_info("metis-link: %s[%d] now\n", __func__, module);
}

/* .text+0x4298, virtual 0010ccc0; real MIGT consumers witness task/flags. */
void register_game_wake_f(metis_game_wake_fn hook)
{
	pr_info("metis-fas: %s now\n", __func__);
	game_wake_hooks = hook;
}

/* .text+0x42dc, virtual 0010cd04. */
void unregister_game_wake_f(void)
{
	game_wake_hooks = NULL;
}

/* .text+0x140c, virtual 00109e34. All returns are ignored, including failed
 * hook/misc/proc registration. No compensation or success inference. */
int metis_link_init(void)
{
	register_trace_android_vh_mutex_wait_start(mi_mutex_wait_start_hook, NULL);
	register_trace_android_vh_alter_rwsem_list_add(mi_rwsem_list_add_hook, NULL);
	register_trace_android_vh_rwsem_write_wait_start(mi_rwsem_write_wait_start_hook, NULL);
	register_trace_android_vh_rwsem_read_wait_start(mi_rwsem_read_wait_start_hook, NULL);
	register_trace_android_vh_binder_restore_priority(mi_binder_restore_vip_hook, NULL);
	register_trace_android_vh_binder_set_priority(mi_binder_set_vip_hook, NULL);
	register_trace_android_vh_record_rwsem_lock_starttime(mi_rwsem_lock_starttime, NULL);
	register_trace_android_vh_record_mutex_lock_starttime(mi_mutex_lock_starttime, NULL);
	register_trace_android_vh_record_rtmutex_lock_starttime(mi_rtmutex_lock_starttime, NULL);
	register_trace_android_vh_record_pcpu_rwsem_starttime(mi_pcpu_rwsem_lock_starttime, NULL);
	register_trace_android_vh_percpu_rwsem_wq_add(mi_pcpu_rwsem_wq_add, NULL);
	register_trace_android_vh_rwsem_read_wait_finish(mi_rwsem_read_wait_finish_hook, NULL);
	register_trace_android_vh_rwsem_write_wait_finish(mi_rwsem_write_wait_finish_hook, NULL);
	register_trace_android_vh_mutex_wait_finish(mi_mutex_wait_finish_hook, NULL);
	register_trace_android_vh_rwsem_read_trylock_failed(mi_rwsem_read_trylock_failed, NULL);
	register_trace_android_vh_rwsem_wake_finish(mi_lock_rwsem_wake_finish_hook, NULL);
	mi_try_to_wake_up_func[1] = metis_link_try_to_wake_up_hook;
	pr_info("metis-link: %s[%d] now\n", "register_rvh_mi_try_to_wake_up", 1);
	register_trace_android_vh_alter_futex_plist_add(mi_alter_futex_plist_add, NULL);
	register_trace_android_vh_do_futex(mi_do_futex, NULL);
	register_trace_android_vh_futex_sleep_start(mi_futex_sleep_start, NULL);
	register_trace_android_vh_futex_wake_this(mi_futex_wake_this, NULL);
	register_trace_android_vh_futex_wait_end(mi_futex_wait_end, NULL);
	misc_register(&metis_link_misc);
	mi_art_mutex_entry = proc_create("show_mi_art_mutex_info", 0664, NULL,
			&mi_art_mutex_debug_proc_ops);
	pr_info("metis-link: LINK: %s: inited!\n", __func__);
	return 0;
}

/* .text+0x342c, virtual 0010be54. Every registered trace hook is deliberately
 * left installed by this original exit body. */
void metis_link_exit(void)
{
	misc_deregister(&metis_link_misc);
	if (mi_art_mutex_entry)
		proc_remove(mi_art_mutex_entry);
	mi_try_to_wake_up_func[1] = NULL;
	pr_info("metis-link: %s[%d] now\n", "unregister_rvh_mi_try_to_wake_up", 1);
	mi_art_mutex_entry = NULL;
	pr_info("metis-link: LINK: %s: exit!\n", __func__);
}

/* .text+0x3cb0, virtual 0010c6d8; fs.h proves file/ioctl ABI, command uses only
 * its low byte. ART owner lookup has no RCU section and counters no lock. */
long metis_link_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	u64 value = 0;
	struct task_struct *owner;
	struct mi_task_struct *state;
	int pid;

	if (!get_mi_fboost_enable())
		return 0;
	if (link_debug)
		pr_info("metis-link: metis link comming: cmd = %d\n", (u8)command);
	switch ((u8)command) {
	case 0:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value))) {
			pr_info("metis-link: read data failed\n");
			break;
		}
		pid = (s32)value;
		mi_art_mutex_set_times++;
		if (pid > 0 && !mi_art_mutex_owner_pid) {
			owner = pid_task(find_vpid(pid), PIDTYPE_PID);
			if (owner && refcount_read(&owner->usage)) {
				get_task_struct(owner);
				if (get_mi_task_struct(owner)) {
					state = get_mi_task_struct(owner);
					if (state) {
						if ((s64)((u64)jiffies - state->deadline_0x290) < 0) {
							if (state->flags_0xc0 & 0x20000)
								goto put_set;
						} else if (state->flags_0xc0 & 0x20000) {
							clean_mi_vip_task_common(owner, 0x20000);
						}
					}
					mi_art_mutex_owner_pid = pid;
					set_mi_vip_task_common(owner, 0x20000, 50);
				}
put_set:
				link_put_task(owner);
			}
		}
		if (link_debug)
			pr_info("metis-link: art mutex contention start, pid = %llu\n", value);
		break;
	case 1:
		mi_art_mutex_unset_times++;
		if (mi_art_mutex_owner_pid > 0) {
			owner = pid_task(find_vpid(mi_art_mutex_owner_pid), PIDTYPE_PID);
			if (owner && refcount_read(&owner->usage)) {
				get_task_struct(owner);
				if (get_mi_task_struct(owner) &&
				    (state = get_mi_task_struct(owner)) &&
				    (state->flags_0xc0 & 0x20000))
					clean_mi_vip_task_common(owner, 0x20000);
				link_put_task(owner);
			}
		}
		mi_art_mutex_owner_pid = 0;
		if (link_debug)
			pr_info("metis-link: art mutex contention end!\n");
		break;
	case 3:
		if (current->tgid == cold_start_app_tgid)
			set_mi_vip_task_common(current, 0x80000, 50);
		break;
	case 4:
		clean_mi_vip_task_common(current, 0x80000);
	}
	return 0;
}

/* .text+0x4154, virtual 0010cb7c, original MOV W0,0; RET. Real fops ABI. */
int metis_link_mmap(struct file *file, struct vm_area_struct *vma)
{
	return 0;
}

/* .text+0x4160, virtual 0010cb88, original MOV W0,0; RET. */
int metis_link_open(struct inode *inode, struct file *file)
{
	return 0;
}

/* .text+0x416c, virtual 0010cb94, original MOV W0,0; RET. */
int metis_link_release(struct inode *inode, struct file *file)
{
	return 0;
}

/* .text+0x4178, virtual 0010cba0. The returned W0 is single_open's result;
 * Ghidra's void signature lost it. Original CFI word is 0x8f07ca55. */
int mi_art_mutex_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, mi_art_mutex_debug_show, pde_data(inode));
}

/* .text+0x41a8, virtual 0010cbd0. Native single_open proves ignored show arg. */
int mi_art_mutex_debug_show(struct seq_file *seq, void *unused)
{
	unsigned long flags;

	spin_lock_irqsave(&mi_art_mutex_lock, flags);
	seq_printf(seq, "\n------------mi art mutex contention statistics------------");
	seq_printf(seq, "\n vtsk set   times : \t %6lld", mi_art_mutex_set_times);
	seq_printf(seq, "\n vtsk unset times : \t %6lld\n", mi_art_mutex_unset_times);
	/* Original DAT_00169f91 is exactly a single newline. */
	seq_printf(seq, "\n");
	spin_unlock_irqrestore(&mi_art_mutex_lock, flags);
	return 0;
}
