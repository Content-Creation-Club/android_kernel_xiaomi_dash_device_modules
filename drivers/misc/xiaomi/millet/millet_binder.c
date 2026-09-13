// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 GitHub @YorokobiMaster */

#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/user_namespace.h>

#include "millet.h"
#include "millet_binder.h"
#include "millet_logic.h"

enum millet_binder_event {
	MILLET_BINDER_OVERFLOW = 0,
	MILLET_BINDER_REPLY = 1,
	MILLET_BINDER_TRANSACTION = 2,
	MILLET_BINDER_WAIT = 3,
};

struct millet_binder_payload {
	u32 event;
	u8 reserved0[60];
	u32 caller_uid;
	u32 caller_pid;
	u32 caller_tid;
	u32 target_pid;
	u8 one_way;
	u8 reserved1[3];
	u32 code;
};

struct millet_binder_stat_payload {
	u8 reserved0[56];
	u32 requester_pid;
	u32 status;
	u32 active;
	u8 reserved1[20];
};

static_assert(sizeof(struct millet_binder_payload) ==
	      sizeof(((struct millet_up_msg *)0)->payload.raw));
static_assert(offsetof(struct millet_binder_payload, caller_uid) == 64);
static_assert(offsetof(struct millet_binder_payload, caller_pid) == 68);
static_assert(offsetof(struct millet_binder_payload, caller_tid) == 72);
static_assert(offsetof(struct millet_binder_payload, target_pid) == 76);
static_assert(offsetof(struct millet_binder_payload, one_way) == 80);
static_assert(offsetof(struct millet_binder_payload, code) == 84);
static_assert(sizeof(struct millet_binder_stat_payload) ==
	      sizeof(((struct millet_up_msg *)0)->payload.raw));
static_assert(offsetof(struct millet_binder_stat_payload, requester_pid) == 56);
static_assert(offsetof(struct millet_binder_stat_payload, status) == 60);
static_assert(offsetof(struct millet_binder_stat_payload, active) == 64);

static u32 millet_binder_uid(struct task_struct *task)
{
	kuid_t kuid;

	rcu_read_lock();
	kuid = task_uid(task);
	rcu_read_unlock();
	return from_kuid_munged(&init_user_ns, kuid);
}

static bool millet_binder_frozen(struct task_struct *task)
{
	/* Current source fields replace stock's build-specific task offsets (B007). */
	return freezing(task) || (READ_ONCE(task->__state) & TASK_FROZEN);
}

static int millet_binder_send(struct task_struct *task,
			      struct millet_up_msg *msg,
			      struct millet_sock *sock)
{
	if (!task || !msg || !sock)
		return -EINVAL;

	millet_prepare_event(msg, MILLET_ID_BINDER);
	if (!millet_binder_frozen(task))
		return 0;
	return millet_sendto_user(task, msg, sock);
}

static void millet_binder_subsystem_init(struct millet_sock *sock)
{
	/* Core owns stock's route/enabled fields (dash-millet.md, M004). */
	(void)sock;
}

static int millet_binder_stat_send(struct task_struct *task,
				   struct millet_up_msg *msg,
				   struct millet_sock *sock)
{
	if (!task || !msg || !sock)
		return -EINVAL;
	millet_prepare_event(msg, MILLET_ID_BINDER_STAT);
	return millet_sendto_user(task, msg, sock);
}

static void millet_binder_stat_init(struct millet_sock *sock)
{
	/* Core owns stock's route/enabled fields (dash-millet.md, M004). */
	(void)sock;
}

static void millet_binder_stat_receive(struct millet_down_msg *msg,
				       unsigned int len)
{
	u32 uid;

	if (!msg || len < sizeof(*msg))
		return;
	memcpy(&uid, &msg->payload.raw[48], sizeof(uid));
	query_binder_app_stat(uid);
}

static void millet_binder_emit(enum millet_binder_event event,
			       struct task_struct *target,
			       struct task_struct *caller, int caller_tid,
			       bool one_way, u32 code)
{
	struct millet_binder_payload *payload;
	struct millet_up_msg msg = { };

	if (!target || !caller)
		return;
	payload = (struct millet_binder_payload *)msg.payload.raw;
	payload->event = event;
	payload->caller_uid = millet_binder_uid(caller);
	payload->caller_pid = task_pid_nr(caller);
	payload->caller_tid = caller_tid;
	payload->target_pid = task_pid_nr(target);
	payload->one_way = one_way;
	payload->code = code;
	msg.uid = millet_binder_uid(target);
	/*
	 * Stock placed two task pointers at wire offsets 0x68/0x70 before its send
	 * callback replaced neither, leaking kernel addresses. Keep those bytes zero
	 * while preserving every observed userspace field (dash-millet-binder.md,
	 * B008).
	 */
	millet_sendmsg(MILLET_ID_BINDER, target, &msg);
}

static void millet_binder_overflow(struct task_struct *owner,
				   struct task_struct *caller,
				   int caller_pid, bool one_way, u32 code)
{
	millet_binder_emit(MILLET_BINDER_OVERFLOW, owner, caller, caller_pid,
			   one_way, code);
}

static void millet_binder_reply(struct task_struct *target,
				struct task_struct *caller,
				int caller_pid, bool one_way, u32 code)
{
	if (millet_binder_uid(target) > frozen_uid_min)
		return;
	millet_binder_emit(MILLET_BINDER_REPLY, target, caller, caller_pid,
			   one_way, code);
}

static void millet_binder_transaction(struct task_struct *target,
				      struct task_struct *caller,
				      int caller_pid, bool one_way, u32 code)
{
	if (millet_binder_uid(target) <= frozen_uid_min ||
	    task_pid_nr(target) == task_pid_nr(caller))
		return;
	millet_binder_emit(MILLET_BINDER_TRANSACTION, target, caller, caller_pid,
			   one_way, code);
}

static void millet_binder_wait(struct task_struct *caller,
			       struct task_struct *target,
			       int target_pid, bool one_way, u32 code)
{
	if (millet_binder_uid(caller) > frozen_uid_min ||
	    task_pid_nr(caller) == task_pid_nr(target))
		return;
	millet_binder_emit(MILLET_BINDER_WAIT, caller, target, target_pid,
			   one_way, code);
}

static void millet_binder_query_stat(u32 uid, struct task_struct *requester,
				     u32 status, int requester_pid,
				     bool active)
{
	struct millet_binder_stat_payload *payload;
	struct millet_up_msg msg = { };

	if (!requester)
		return;
	payload = (struct millet_binder_stat_payload *)msg.payload.raw;
	msg.uid = uid;
	payload->requester_pid = requester_pid;
	payload->status = status;
	payload->active = active;
	/* Stock leaked requester at wire offset 0x68; B008 keeps it zero. */
	millet_sendmsg(MILLET_ID_BINDER_STAT, requester, &msg);
}

static const struct millet_binder_hooks millet_binder_hook_set = {
	.async_space_multiplier = 3,
	.warn_ahead_space = 0x20000,
	.overflow = millet_binder_overflow,
	.reply = millet_binder_reply,
	.transaction = millet_binder_transaction,
	.wait = millet_binder_wait,
	.query_stat = millet_binder_query_stat,
};

static int __init millet_binder_init(void)
{
	int ret;

	/* Stock ignored every registration failure and had no exit; unwind safely (B002). */

	ret = oem_register_binder_hook(&millet_binder_hook_set);
	if (ret)
		return ret;
	ret = register_millet_hook(MILLET_ID_BINDER, NULL, millet_binder_send,
				   millet_binder_subsystem_init);
	if (ret)
		goto err_oem;
	ret = init_millet_subsystem(MILLET_ID_BINDER);
	if (ret)
		goto err_binder;
	ret = register_millet_hook(MILLET_ID_BINDER_STAT,
				   millet_binder_stat_receive,
				   millet_binder_stat_send,
				   millet_binder_stat_init);
	if (ret)
		goto err_binder;
	ret = init_millet_subsystem(MILLET_ID_BINDER_STAT);
	if (ret)
		goto err_stat;
	return 0;

err_stat:
	unregister_millet_hook(MILLET_ID_BINDER_STAT);
err_binder:
	unregister_millet_hook(MILLET_ID_BINDER);
err_oem:
	oem_unregister_binder_hook(&millet_binder_hook_set);
	return ret;
}

static void __exit millet_binder_exit(void)
{
	oem_unregister_binder_hook(&millet_binder_hook_set);
	unregister_millet_hook(MILLET_ID_BINDER_STAT);
	unregister_millet_hook(MILLET_ID_BINDER);
}

module_init(millet_binder_init);
module_exit(millet_binder_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GitHub @YorokobiMaster");
MODULE_DESCRIPTION("Xiaomi Millet frozen-task Binder reporting");
