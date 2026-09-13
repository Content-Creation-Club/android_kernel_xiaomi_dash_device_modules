// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 GitHub @YorokobiMaster */

#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/user_namespace.h>
#include <trace/hooks/signal.h>

#include "millet.h"
#include "millet_logic.h"

static atomic_t millet_sig_last_pid = ATOMIC_INIT(0);

static bool millet_sig_task_frozen(struct task_struct *task)
{
	/*
	 * Intentional stock difference (dash-millet.md, M007): stock reads task
	 * and cgroup internals by build-specific offsets. Use the current freezer
	 * API plus the same named TASK_FROZEN state used by frozen(), which is not
	 * exported to vendor modules in this tree.
	 */
	return freezing(task) || (READ_ONCE(task->__state) & TASK_FROZEN);
}

static int millet_sig_send(struct task_struct *task, struct millet_up_msg *msg,
			   struct millet_sock *sock)
{
	kuid_t kuid;
	u32 pid;
	u32 uid;

	if (!task || !msg || !sock)
		return -EINVAL;

	pid = task_pid_nr(task);
	rcu_read_lock();
	kuid = task_uid(task);
	rcu_read_unlock();
	uid = from_kuid_munged(&init_user_ns, kuid);
	millet_prepare_sig_event(msg, uid, pid);

	if (!millet_sig_task_frozen(task))
		return 0;
	/*
	 * Intentional stock difference (dash-millet.md, M008): stock races on a
	 * shared PID integer. Atomic exchange preserves global consecutive-PID
	 * suppression without undefined concurrent access.
	 */
	if (atomic_xchg(&millet_sig_last_pid, pid) == pid)
		return 0;
	return millet_sendto_user(task, msg, sock);
}

static void millet_sig_subsystem_init(struct millet_sock *sock)
{
	/*
	 * Intentional stock difference (dash-millet.md, M004): stock writes
	 * core-private route/enabled fields here; core now owns that state.
	 */
	(void)sock;
	atomic_set(&millet_sig_last_pid, 0);
}

void millet_sig(void *unused, int sig, struct task_struct *killer,
		struct task_struct *destination)
{
	struct millet_up_msg msg = { };

	(void)unused;
	(void)killer;
	if (!destination || !millet_signal_selected(sig))
		return;
	millet_sendmsg(MILLET_ID_SIG, destination, &msg);
}
EXPORT_SYMBOL_GPL(millet_sig);

static int __init millet_sig_init(void)
{
	int ret;

	ret = register_millet_hook(MILLET_ID_SIG, NULL, millet_sig_send,
				   millet_sig_subsystem_init);
	if (ret)
		return ret;
	ret = init_millet_subsystem(MILLET_ID_SIG);
	if (ret)
		goto err_core;
	ret = register_trace_android_vh_do_send_sig_info(millet_sig, NULL);
	if (ret)
		goto err_core;
	return 0;

err_core:
	unregister_millet_hook(MILLET_ID_SIG);
	return ret;
}

static void __exit millet_sig_exit(void)
{
	/*
	 * Intentional stock difference (dash-millet.md, M003): stock has no exit
	 * and leaves a trace callback into unloadable module text.
	 */
	unregister_trace_android_vh_do_send_sig_info(millet_sig, NULL);
	unregister_millet_hook(MILLET_ID_SIG);
}

module_init(millet_sig_init);
module_exit(millet_sig_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GitHub @YorokobiMaster");
MODULE_DESCRIPTION("Xiaomi Millet frozen-task signal reporting");
