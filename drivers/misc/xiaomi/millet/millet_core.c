// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 GitHub @YorokobiMaster */

#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "millet.h"
#include "millet_logic.h"

#define MILLET_VERSION 1

struct millet_callbacks {
	millet_recv_cb recv;
	millet_send_cb send;
	millet_init_cb init;
};

struct millet_subsystem {
	/* Protects transport state and send statistics. */
	spinlock_t lock;
	struct millet_callbacks registered;
	struct millet_callbacks __rcu *callbacks;
	const char *name;
	u32 route_type;
	u32 portid;
	bool handshaken;
	bool enabled;
	u64 send_success;
	u64 send_failure;
	u64 runtime_ns;
};

struct millet_sock {
	struct sock *netlink;
	struct millet_subsystem subsystem[MILLET_ID_INVALID];
};

static const char *const millet_names[MILLET_ID_INVALID + 1] = {
	[MILLET_ID_NULL] = "NULL",
	[MILLET_ID_SIG] = "SIG",
	[MILLET_ID_BINDER] = "BINDER",
	[MILLET_ID_BINDER_STAT] = "BINDER_STAT",
	[MILLET_ID_MEM] = "MEM",
	[MILLET_ID_PKG] = "PKG",
	[MILLET_ID_HANDSHAKE] = "HANDSHK",
	[MILLET_ID_INVALID] = "invalid",
};

static DEFINE_MUTEX(millet_hook_lock);
static struct millet_sock millet_sock_state;
static struct proc_dir_entry *millet_proc_dir;

unsigned int frozen_uid_min = MILLET_APP_UID_MIN;
EXPORT_SYMBOL_GPL(frozen_uid_min);
module_param(frozen_uid_min, uint, 0644);

unsigned long binder_warn_ahead_space = 0x20000;
EXPORT_SYMBOL_GPL(binder_warn_ahead_space);
module_param(binder_warn_ahead_space, ulong, 0644);

bool millet_binder_switch = true;
EXPORT_SYMBOL_GPL(millet_binder_switch);
module_param(millet_binder_switch, bool, 0644);

static int millet_freeze_switch;
module_param(millet_freeze_switch, int, 0644);

unsigned int millet_debug;
EXPORT_SYMBOL_GPL(millet_debug);
module_param(millet_debug, uint, 0644);

static void millet_set_destination(struct millet_subsystem *subsystem, u32 portid)
{
	unsigned long flags;

	spin_lock_irqsave(&subsystem->lock, flags);
	subsystem->portid = portid;
	subsystem->handshaken = true;
	spin_unlock_irqrestore(&subsystem->lock, flags);
}

int millet_sendto_user(struct task_struct *task, struct millet_up_msg *msg,
		       struct millet_sock *sock)
{
	struct millet_subsystem *route;
	struct millet_subsystem *subsystem;
	struct millet_up_msg wire;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct timespec64 now;
	unsigned long flags;
	u32 portid;
	u32 route_type;
	int ret;

	(void)task;
	if (!msg || !sock || !millet_valid_type(msg->type) ||
	    !millet_valid_kind(msg->kind))
		return -EINVAL;

	subsystem = &sock->subsystem[msg->type];
	spin_lock_irqsave(&subsystem->lock, flags);
	route_type = subsystem->route_type;
	spin_unlock_irqrestore(&subsystem->lock, flags);
	if (!millet_valid_type(route_type))
		route_type = msg->type;

	route = &sock->subsystem[route_type];
	spin_lock_irqsave(&route->lock, flags);
	portid = route->portid;
	ret = route->enabled && route->handshaken ? 0 : -ENOTCONN;
	spin_unlock_irqrestore(&route->lock, flags);
	if (ret)
		return ret;

	skb = nlmsg_new(sizeof(wire), GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	nlh = nlmsg_put(skb, 0, 0, 0, sizeof(wire), 0);
	if (!nlh) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}

	wire = *msg;
	/*
	 * Stock routes HANDSHK through SIG and BINDER_STAT through BINDER, and
	 * exposes that route at wire offset 0x04. Core owns the observed mapping
	 * instead of satellites writing core-private state (dash-millet.md, M004).
	 */
	wire.route = route_type;
	wire.source = MILLET_UP_SOURCE;
	wire.destination = MILLET_UP_DESTINATION;
	ktime_get_ts64(&now);
	wire.timestamp_sec = now.tv_sec;
	wire.timestamp_nsec = now.tv_nsec;
	memcpy(nlmsg_data(nlh), &wire, sizeof(wire));

	ret = netlink_unicast(sock->netlink, skb, portid, MSG_DONTWAIT);
	/*
	 * Intentional stock difference (dash-millet.md, M017): stock collapses
	 * every failure to -1. Preserve the real errno while keeping success 0.
	 */
	return ret < 0 ? ret : 0;
}
EXPORT_SYMBOL_GPL(millet_sendto_user);

int millet_sendmsg(unsigned int type, struct task_struct *task,
		   struct millet_up_msg *msg)
{
	struct millet_callbacks *callbacks;
	struct millet_subsystem *subsystem;
	millet_send_cb send;
	unsigned long flags;
	u64 start;
	int ret;

	if (!millet_valid_type(type) || !task || !msg)
		return -EINVAL;

	subsystem = &millet_sock_state.subsystem[type];
	start = ktime_get_ns();
	rcu_read_lock();
	callbacks = rcu_dereference(subsystem->callbacks);
	send = callbacks ? callbacks->send : NULL;
	ret = send ? send(task, msg, &millet_sock_state) : -ENODEV;
	rcu_read_unlock();

	spin_lock_irqsave(&subsystem->lock, flags);
	/*
	 * Intentional stock difference (dash-millet.md, M005): stock's counter
	 * path does not count the normal zero return as success. Count API success
	 * directly so /proc/millet/millet_stat describes delivered calls.
	 */
	if (ret)
		subsystem->send_failure++;
	else
		subsystem->send_success++;
	subsystem->runtime_ns += ktime_get_ns() - start;
	spin_unlock_irqrestore(&subsystem->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(millet_sendmsg);

int register_millet_hook(unsigned int type, millet_recv_cb recv,
			 millet_send_cb send, millet_init_cb init)
{
	struct millet_subsystem *subsystem;
	int ret = 0;

	if (!millet_valid_type(type))
		return -EINVAL;

	subsystem = &millet_sock_state.subsystem[type];
	mutex_lock(&millet_hook_lock);
	/*
	 * Intentional stock difference (dash-millet.md, M004): stock publishes
	 * callbacks one at a time and leaves partial registration on collision.
	 * Publish the complete KCFI-typed callback set atomically under one lock.
	 */
	if (rcu_access_pointer(subsystem->callbacks)) {
		ret = -EBUSY;
	} else {
		subsystem->registered.recv = recv;
		subsystem->registered.send = send;
		subsystem->registered.init = init;
		rcu_assign_pointer(subsystem->callbacks, &subsystem->registered);
	}
	mutex_unlock(&millet_hook_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(register_millet_hook);

int unregister_millet_hook(unsigned int type)
{
	struct millet_subsystem *subsystem;

	if (!millet_valid_type(type))
		return -EINVAL;

	subsystem = &millet_sock_state.subsystem[type];
	mutex_lock(&millet_hook_lock);
	RCU_INIT_POINTER(subsystem->callbacks, NULL);
	/*
	 * Intentional stock difference (dash-millet.md, M003): stock clears
	 * callbacks without waiting for callers. Drain RCU readers before a
	 * satellite module can release its callback text or register again.
	 */
	synchronize_rcu();
	memset(&subsystem->registered, 0, sizeof(subsystem->registered));
	mutex_unlock(&millet_hook_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(unregister_millet_hook);

int init_millet_subsystem(unsigned int type)
{
	struct millet_callbacks *callbacks;
	struct millet_subsystem *subsystem;
	millet_init_cb init;
	unsigned long flags;

	if (!millet_valid_type(type))
		return -EINVAL;

	subsystem = &millet_sock_state.subsystem[type];
	mutex_lock(&millet_hook_lock);
	callbacks = rcu_dereference_protected(subsystem->callbacks,
					      lockdep_is_held(&millet_hook_lock));
	init = callbacks ? callbacks->init : NULL;
	if (init)
		init(&millet_sock_state);
	mutex_unlock(&millet_hook_lock);

	/*
	 * Intentional stock difference (dash-millet.md, M004): stock satellite
	 * init callbacks write core-private route/enabled fields. Core owns those
	 * fields here; the callback remains for source compatibility.
	 */
	spin_lock_irqsave(&subsystem->lock, flags);
	subsystem->route_type = millet_route_for_type(type);
	subsystem->enabled = true;
	spin_unlock_irqrestore(&subsystem->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(init_millet_subsystem);

static void millet_recv_handler(struct sk_buff *skb)
{
	struct millet_callbacks *callbacks;
	struct millet_down_msg *msg;
	struct millet_subsystem *subsystem;
	struct millet_up_msg ack = { };
	struct nlmsghdr *nlh;
	millet_recv_cb recv;
	u32 sender_uid;

	if (!skb || skb->len < NLMSG_HDRLEN)
		return;

	sender_uid = from_kuid_munged(&init_user_ns, NETLINK_CB(skb).creds.uid);
	if (sender_uid > 1000)
		return;

	nlh = nlmsg_hdr(skb);
	if (!nlmsg_ok(nlh, skb->len) || nlmsg_len(nlh) < sizeof(*msg))
		return;

	msg = nlmsg_data(nlh);
	if (!millet_down_valid(msg, nlmsg_len(nlh)))
		return;
	subsystem = &millet_sock_state.subsystem[msg->type];

	if (msg->command == MILLET_CMD_HANDSHAKE) {
		/*
		 * Intentional stock difference (dash-millet.md, M002): stock trusts
		 * spoofable nlmsg_pid. Bind replies to the authenticated sender port.
		 */
		millet_set_destination(subsystem, NETLINK_CB(skb).portid);
		ack.type = msg->type;
		ack.kind = MILLET_MSG_ACK;
		millet_sendto_user(current, &ack, &millet_sock_state);
		return;
	}

	rcu_read_lock();
	callbacks = rcu_dereference(subsystem->callbacks);
	recv = callbacks ? callbacks->recv : NULL;
	if (recv)
		recv(msg, sizeof(*msg));
	rcu_read_unlock();
}

static int millet_stat_show(struct seq_file *m, void *unused)
{
	u64 total_success = 0;
	u64 total_failure = 0;
	u64 total_runtime = 0;
	unsigned int type;

	seq_puts(m, "-----------------------------\n\n");
	for (type = MILLET_ID_SIG; type < MILLET_ID_INVALID; type++) {
		struct millet_subsystem *subsystem =
			&millet_sock_state.subsystem[type];
		unsigned long flags;
		u64 success;
		u64 failure;
		u64 runtime;

		spin_lock_irqsave(&subsystem->lock, flags);
		success = subsystem->send_success;
		failure = subsystem->send_failure;
		runtime = subsystem->runtime_ns;
		spin_unlock_irqrestore(&subsystem->lock, flags);
		if (!success && !failure)
			continue;

		total_success += success;
		total_failure += failure;
		total_runtime += runtime;
		seq_printf(m, "name: %s mod id %u:\n", subsystem->name, type);
		seq_printf(m, "send suc: %llu\n", success);
		seq_printf(m, "send fail: %llu\n", failure);
		seq_printf(m, "runtime: %llu us\n\n", runtime / NSEC_PER_USEC);
	}
	seq_puts(m, "---------total info--------------------\n");
	seq_printf(m, "send_suc: %llu\n", total_success);
	seq_printf(m, "send_fail: %llu\n", total_failure);
	seq_printf(m, "runtime: %llu us\n", total_runtime / NSEC_PER_USEC);
	return 0;
}

static int millet_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, millet_stat_show, NULL);
}

static ssize_t millet_stat_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char input[16];
	size_t len = min(count, sizeof(input) - 1);
	unsigned int type;
	int value;

	if (!len || copy_from_user(input, buf, len))
		return -EFAULT;
	input[len] = '\0';
	if (kstrtoint(input, 0, &value))
		return -EINVAL;
	if (value != 1)
		return count;

	for (type = MILLET_ID_SIG; type < MILLET_ID_INVALID; type++) {
		struct millet_subsystem *subsystem =
			&millet_sock_state.subsystem[type];
		unsigned long flags;

		spin_lock_irqsave(&subsystem->lock, flags);
		subsystem->send_success = 0;
		subsystem->send_failure = 0;
		subsystem->runtime_ns = 0;
		spin_unlock_irqrestore(&subsystem->lock, flags);
	}
	return count;
}

static const struct proc_ops millet_stat_ops = {
	.proc_open = millet_stat_open,
	.proc_read = seq_read,
	.proc_write = millet_stat_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int millet_version_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "%d\n", MILLET_VERSION);
	return 0;
}

static int millet_version_open(struct inode *inode, struct file *file)
{
	return single_open(file, millet_version_show, NULL);
}

static const struct proc_ops millet_version_ops = {
	.proc_open = millet_version_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init millet_core_init(void)
{
	struct netlink_kernel_cfg cfg = {
		.input = millet_recv_handler,
	};
	unsigned int type;

	for (type = MILLET_ID_NULL; type < MILLET_ID_INVALID; type++) {
		spin_lock_init(&millet_sock_state.subsystem[type].lock);
		millet_sock_state.subsystem[type].name = millet_names[type];
		millet_sock_state.subsystem[type].route_type = type;
	}

	millet_sock_state.netlink = netlink_kernel_create(&init_net, MILLET_NETLINK_PROTOCOL, &cfg);
	if (!millet_sock_state.netlink)
		return -ENOMEM;

	millet_proc_dir = proc_mkdir("millet", NULL);
	if (!millet_proc_dir)
		goto err_netlink;
	if (!proc_create("millet_stat", 0644, millet_proc_dir, &millet_stat_ops))
		goto err_proc;
	if (!proc_create("version", 0644, millet_proc_dir, &millet_version_ops))
		goto err_proc;
	return 0;

err_proc:
	/*
	 * Intentional stock difference (dash-millet.md, M003): stock reports
	 * successful init after proc failures and leaks partial state. Fail and
	 * unwind so no incomplete core transport is advertised.
	 */
	proc_remove(millet_proc_dir);
	millet_proc_dir = NULL;
err_netlink:
	netlink_kernel_release(millet_sock_state.netlink);
	millet_sock_state.netlink = NULL;
	return -ENOMEM;
}

static void __exit millet_core_exit(void)
{
	proc_remove(millet_proc_dir);
	millet_proc_dir = NULL;
	netlink_kernel_release(millet_sock_state.netlink);
	millet_sock_state.netlink = NULL;
}

module_init(millet_core_init);
module_exit(millet_core_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GitHub @YorokobiMaster");
MODULE_DESCRIPTION("Xiaomi Millet protocol core");
