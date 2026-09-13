// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 GitHub @YorokobiMaster */

#include <linux/errno.h>
#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <drivers/android/binder_internal.h>
#include <trace/hooks/binder.h>

#include "millet.h"
#include "millet_binder.h"

#define MILLET_BINDER_DESC_MAX 140
#define MILLET_BINDER_DESC_OFFSET 16

static DEFINE_MUTEX(millet_binder_hook_lock);
static const struct millet_binder_hooks __rcu *millet_binder_hooks;
static struct hlist_head *millet_binder_procs;
static struct mutex *millet_binder_procs_lock;

static u32 millet_task_uid(struct task_struct *task)
{
	kuid_t kuid;

	rcu_read_lock();
	kuid = task_uid(task);
	rcu_read_unlock();
	return from_kuid_munged(&init_user_ns, kuid);
}

static bool millet_binder_task_frozen(struct task_struct *task)
{
	/* Current source fields replace stock's build-specific task offsets (B007). */
	return freezing(task) || (READ_ONCE(task->__state) & TASK_FROZEN);
}

static int millet_binder_proc_status(struct binder_proc *proc)
{
	struct binder_transaction *transaction;
	struct binder_thread *thread;
	struct binder_work *work;
	struct rb_node *node;
	int status = 0;

	spin_lock(&proc->inner_lock);
	if (!millet_binder_switch && !list_empty(&proc->todo)) {
		status = 3;
		goto out;
	}
	if (millet_binder_switch) {
		list_for_each_entry(work, &proc->todo, entry) {
			if (work->type != BINDER_WORK_TRANSACTION)
				continue;
			transaction = container_of(work, struct binder_transaction,
						   work);
			spin_lock(&transaction->lock);
			if (!transaction->to_thread && transaction->need_reply)
				status = 3;
			spin_unlock(&transaction->lock);
			if (status)
				goto out;
		}
	}

	for (node = rb_first(&proc->threads); node; node = rb_next(node)) {
		thread = rb_entry(node, struct binder_thread, rb_node);
		if (!thread->task)
			continue;

		if (!millet_binder_switch && !list_empty(&thread->todo)) {
			status = 2;
			goto out;
		}
		if (millet_binder_switch) {
			list_for_each_entry(work, &thread->todo, entry) {
				if (work->type != BINDER_WORK_TRANSACTION)
					continue;
				transaction = container_of(work,
							   struct binder_transaction,
							   work);
				spin_lock(&transaction->lock);
				if (transaction->to_thread == thread &&
				    transaction->need_reply)
					status = 2;
				spin_unlock(&transaction->lock);
				if (status)
					goto out;
			}
		}

		transaction = thread->transaction_stack;
		if (!transaction)
			continue;
		spin_lock(&transaction->lock);
		if (transaction->to_thread == thread &&
		    (!millet_binder_switch || transaction->need_reply))
			status = 4;
		spin_unlock(&transaction->lock);
		if (status)
			goto out;
	}
out:
	spin_unlock(&proc->inner_lock);
	return status;
}

void query_binder_app_stat(u32 uid)
{
	const struct millet_binder_hooks *hooks;
	struct binder_proc *proc;
	bool active = false;

	mutex_lock(&millet_binder_hook_lock);
	hooks = rcu_dereference_protected(millet_binder_hooks, 1);
	if (!hooks || !hooks->query_stat || !millet_binder_procs ||
	    !millet_binder_procs_lock) {
		mutex_unlock(&millet_binder_hook_lock);
		return;
	}

	mutex_lock(millet_binder_procs_lock);
	hlist_for_each_entry(proc, millet_binder_procs, proc_node) {
		if (proc->tsk && millet_task_uid(proc->tsk) == uid &&
		    millet_binder_proc_status(proc)) {
			active = true;
			break;
		}
	}
	mutex_unlock(millet_binder_procs_lock);
	hooks->query_stat(uid, current, 0, task_pid_nr(current), active);
	mutex_unlock(&millet_binder_hook_lock);
}
EXPORT_SYMBOL_GPL(query_binder_app_stat);

static void millet_binder_call_event(millet_binder_event_cb callback,
				     struct task_struct *primary,
				     struct task_struct *peer, int pid,
				     bool one_way, u32 code)
{
	if (!callback || !primary || !peer)
		return;
	/* Stock split task references across both modules; one owner is safer (B004). */
	get_task_struct(primary);
	get_task_struct(peer);
	callback(primary, peer, pid, one_way, code);
	put_task_struct(peer);
	put_task_struct(primary);
}

static void binder_alloc_hook(void *unused, size_t size,
			      size_t *free_async_space, int is_async,
			      bool *should_fail)
{
	const struct millet_binder_hooks *hooks;
	struct binder_alloc *alloc;
	struct binder_proc *proc;
	size_t warning_space;
	bool overflow;

	(void)unused;
	(void)should_fail;
	if (!is_async || !free_async_space)
		return;

	rcu_read_lock();
	hooks = rcu_dereference(millet_binder_hooks);
	if (!hooks || !hooks->overflow || !hooks->async_space_multiplier) {
		rcu_read_unlock();
		return;
	}

	/* Source-level sizes replace stock's binder_buffer constant (B003). */
	overflow = check_add_overflow(size, sizeof(struct binder_buffer),
				      &warning_space) ||
		   check_mul_overflow(warning_space,
				      hooks->async_space_multiplier,
				      &warning_space) ||
		   *free_async_space < warning_space ||
		   *free_async_space < hooks->warn_ahead_space;
	if (overflow) {
		alloc = container_of(free_async_space, struct binder_alloc,
				     free_async_space);
		proc = container_of(alloc, struct binder_proc, alloc);
		millet_binder_call_event(hooks->overflow, proc->tsk, current,
					 task_pid_nr(current), false, 0);
	}
	rcu_read_unlock();
}

static void millet_binder_reply(void *unused, struct binder_proc *target_proc,
				struct binder_proc *proc,
				struct binder_thread *thread,
				struct binder_transaction_data *transaction)
{
	const struct millet_binder_hooks *hooks;

	(void)unused;
	rcu_read_lock();
	hooks = rcu_dereference(millet_binder_hooks);
	if (hooks)
		millet_binder_call_event(hooks->reply, target_proc->tsk, proc->tsk,
					 thread->pid,
					 transaction->flags & TF_ONE_WAY,
					 transaction->code);
	rcu_read_unlock();
}

static void millet_binder_log_transaction(struct binder_proc *target_proc,
					  struct binder_proc *proc,
					  struct binder_thread *thread,
					  struct binder_transaction_data *transaction)
{
	char descriptor[MILLET_BINDER_DESC_MAX + 1] = { };
	u8 parcel[MILLET_BINDER_DESC_MAX] = { };
	size_t copied;
	size_t i;
	size_t out = 0;

	if (!millet_debug || !(transaction->flags & TF_ONE_WAY) ||
	    !target_proc->tsk || millet_task_uid(target_proc->tsk) <= 10000 ||
	    !millet_binder_task_frozen(target_proc->tsk))
		return;

	copied = min_t(size_t, transaction->data_size, sizeof(parcel));
	if (copied <= MILLET_BINDER_DESC_OFFSET ||
	    copy_from_user(parcel,
			   u64_to_user_ptr(transaction->data.ptr.buffer), copied))
		return;
	for (i = MILLET_BINDER_DESC_OFFSET; i < copied && out < sizeof(descriptor) - 1;
	     i += 2) {
		if (!parcel[i])
			break;
		descriptor[out++] = parcel[i];
	}
	pr_info("millet_kernel calleruid:%u callerPid:%d targetUid:%u targetPid:%d code:%u desc:%s\n",
		millet_task_uid(proc->tsk), thread->pid,
		millet_task_uid(target_proc->tsk), task_pid_nr(target_proc->tsk),
		transaction->code, descriptor);
}

static void millet_binder_transaction(void *unused,
				      struct binder_proc *target_proc,
				      struct binder_proc *proc,
				      struct binder_thread *thread,
				      struct binder_transaction_data *transaction)
{
	const struct millet_binder_hooks *hooks;

	(void)unused;
	rcu_read_lock();
	hooks = rcu_dereference(millet_binder_hooks);
	if (hooks)
		millet_binder_call_event(hooks->transaction, target_proc->tsk,
					 proc->tsk, thread->pid,
					 transaction->flags & TF_ONE_WAY,
					 transaction->code);
	rcu_read_unlock();
	millet_binder_log_transaction(target_proc, proc, thread, transaction);
}

static void binder_wait_hook(void *unused, bool do_proc_work,
			     struct binder_thread *thread,
			     struct binder_proc *proc)
{
	const struct millet_binder_hooks *hooks;
	struct binder_transaction *transaction;
	struct binder_thread *from;
	struct task_struct *caller = NULL;
	bool one_way = false;
	u32 code = 0;

	(void)unused;
	(void)do_proc_work;
	transaction = thread->transaction_stack;
	if (!transaction)
		return;

	spin_lock(&transaction->lock);
	from = transaction->from;
	if (from && from->task) {
		caller = from->task;
		get_task_struct(caller);
		one_way = transaction->flags & TF_ONE_WAY;
		code = transaction->code;
	}
	spin_unlock(&transaction->lock);
	if (!caller)
		return;

	rcu_read_lock();
	hooks = rcu_dereference(millet_binder_hooks);
	if (hooks && hooks->wait)
		hooks->wait(caller, proc->tsk, thread->pid, one_way, code);
	rcu_read_unlock();
	put_task_struct(caller);
}

static void millet_binder_preset(void *unused, struct hlist_head *head,
				 struct mutex *lock, struct binder_proc *proc)
{
	(void)unused;
	(void)proc;
	if (!READ_ONCE(millet_binder_procs)) {
		WRITE_ONCE(millet_binder_procs, head);
		WRITE_ONCE(millet_binder_procs_lock, lock);
	}
}

int oem_register_binder_hook(const struct millet_binder_hooks *hooks)
{
	int ret = 0;

	/* Stock overwrote this table without exclusion; reject duplicate owners (B001). */

	if (!hooks)
		return -EINVAL;
	mutex_lock(&millet_binder_hook_lock);
	if (rcu_access_pointer(millet_binder_hooks))
		ret = -EBUSY;
	else
		rcu_assign_pointer(millet_binder_hooks, hooks);
	mutex_unlock(&millet_binder_hook_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(oem_register_binder_hook);

void oem_unregister_binder_hook(const struct millet_binder_hooks *hooks)
{
	/* Stock had no detach path; drain every callback before unload (B001). */
	mutex_lock(&millet_binder_hook_lock);
	if (rcu_access_pointer(millet_binder_hooks) == hooks) {
		RCU_INIT_POINTER(millet_binder_hooks, NULL);
		synchronize_rcu();
	}
	mutex_unlock(&millet_binder_hook_lock);
}
EXPORT_SYMBOL_GPL(oem_unregister_binder_hook);

static int __init binder_gki_init(void)
{
	int ret;

	/* Stock ignored hook failures and had no exit; unwind all partial state (B002). */

	ret = register_trace_android_vh_binder_alloc_new_buf_locked(binder_alloc_hook, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_binder_reply(millet_binder_reply, NULL);
	if (ret)
		goto err_alloc;
	ret = register_trace_android_vh_binder_trans(millet_binder_transaction, NULL);
	if (ret)
		goto err_reply;
	ret = register_trace_android_vh_binder_wait_for_work(binder_wait_hook, NULL);
	if (ret)
		goto err_trans;
	ret = register_trace_android_vh_binder_preset(millet_binder_preset, NULL);
	if (ret)
		goto err_wait;
	return 0;

err_wait:
	unregister_trace_android_vh_binder_wait_for_work(binder_wait_hook, NULL);
err_trans:
	unregister_trace_android_vh_binder_trans(millet_binder_transaction, NULL);
err_reply:
	unregister_trace_android_vh_binder_reply(millet_binder_reply, NULL);
err_alloc:
	unregister_trace_android_vh_binder_alloc_new_buf_locked(binder_alloc_hook, NULL);
	return ret;
}

static void __exit binder_gki_exit(void)
{
	unregister_trace_android_vh_binder_preset(millet_binder_preset, NULL);
	unregister_trace_android_vh_binder_wait_for_work(binder_wait_hook, NULL);
	unregister_trace_android_vh_binder_trans(millet_binder_transaction, NULL);
	unregister_trace_android_vh_binder_reply(millet_binder_reply, NULL);
	unregister_trace_android_vh_binder_alloc_new_buf_locked(binder_alloc_hook, NULL);
	WRITE_ONCE(millet_binder_procs, NULL);
	WRITE_ONCE(millet_binder_procs_lock, NULL);
}

module_init(binder_gki_init);
module_exit(binder_gki_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GitHub @YorokobiMaster");
MODULE_DESCRIPTION("Xiaomi Millet Binder vendor-hook bridge");
