// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 GitHub @YorokobiMaster */

#include <linux/errno.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/ip.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/user_namespace.h>
#include <net/inet_sock.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include "millet.h"
#include "millet_logic.h"

#define MILLET_PKG_PRIORITY 226

static DEFINE_SPINLOCK(millet_pkg_lock);
static millet_u32 millet_pkg_uids[MILLET_UID_SLOTS];

static void millet_pkg_recv(struct millet_down_msg *msg, unsigned int len)
{
	unsigned int operation;
	unsigned long flags;
	int ret = 0;

	if (!msg || len < sizeof(*msg))
		return;

	operation = msg->payload.pkg.operation;
	spin_lock_irqsave(&millet_pkg_lock, flags);
	/*
	 * Intentional stock difference (dash-millet.md, M010): stock mixes
	 * unlocked table updates with atomic packet-side consumes. One spinlock
	 * makes add/remove/clear/consume one coherent one-shot set.
	 */
	switch (operation) {
	case MILLET_PKG_ADD:
		ret = millet_uid_add(millet_pkg_uids, ARRAY_SIZE(millet_pkg_uids),
				     msg->payload.pkg.uid);
		break;
	case MILLET_PKG_REMOVE:
		millet_uid_remove(millet_pkg_uids, ARRAY_SIZE(millet_pkg_uids),
				  msg->payload.pkg.uid);
		break;
	case MILLET_PKG_CLEAR:
		millet_uid_clear(millet_pkg_uids, ARRAY_SIZE(millet_pkg_uids));
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&millet_pkg_lock, flags);
	if (ret < 0)
		pr_warn_ratelimited("millet_pkg: UID table full\n");
}

static int millet_pkg_send(struct task_struct *task, struct millet_up_msg *msg,
			   struct millet_sock *sock)
{
	if (!task || !msg || !sock)
		return -EINVAL;
	millet_prepare_pkg_event(msg);
	return millet_sendto_user(task, msg, sock);
}

static void millet_pkg_subsystem_init(struct millet_sock *sock)
{
	/*
	 * Intentional stock difference (dash-millet.md, M004): stock writes
	 * core-private route/enabled fields here; core now owns that state.
	 */
	(void)sock;
}

static bool millet_pkg_consume_uid(u32 uid)
{
	unsigned long flags;
	bool matched;

	spin_lock_irqsave(&millet_pkg_lock, flags);
	matched = millet_uid_consume(millet_pkg_uids,
				     ARRAY_SIZE(millet_pkg_uids), uid);
	spin_unlock_irqrestore(&millet_pkg_lock, flags);
	return matched;
}

static unsigned int millet_pkg_report(struct sk_buff *skb,
				      const struct nf_hook_state *state)
{
	struct millet_up_msg msg = { };
	struct sock *sk;
	kuid_t kuid;
	u32 uid;

	if (!skb || !state || !state->net)
		return NF_ACCEPT;
	sk = skb_to_full_sk(skb);
	if (!sk || !sk_fullsock(sk))
		return NF_ACCEPT;

	kuid = sock_net_uid(state->net, sk);
	uid = from_kuid_munged(&init_user_ns, kuid);
	if (!millet_pkg_uid_reportable(uid) || !millet_pkg_consume_uid(uid))
		return NF_ACCEPT;

	msg.payload.pkg.uid = uid;
	millet_sendmsg(MILLET_ID_PKG, current, &msg);
	return NF_ACCEPT;
}

static unsigned int millet_pkg_ipv4_in(void *priv, struct sk_buff *skb,
				       const struct nf_hook_state *state)
{
	struct iphdr buffer;
	const struct iphdr *ip;

	(void)priv;
	if (!skb)
		return NF_ACCEPT;
	ip = skb_header_pointer(skb, skb_network_offset(skb), sizeof(buffer),
				&buffer);
	if (!ip || ip->protocol != IPPROTO_TCP)
		return NF_ACCEPT;
	return millet_pkg_report(skb, state);
}

static unsigned int millet_pkg_ipv6_in(void *priv, struct sk_buff *skb,
				       const struct nf_hook_state *state)
{
	unsigned int offset = 0;
	unsigned short fragoff = 0;

	(void)priv;
	if (!skb || ipv6_find_hdr(skb, &offset, -1, &fragoff, NULL) != IPPROTO_TCP)
		return NF_ACCEPT;
	return millet_pkg_report(skb, state);
}

static unsigned int millet_pkg_out(void *priv, struct sk_buff *skb,
				   const struct nf_hook_state *state)
{
	(void)priv;
	(void)skb;
	(void)state;
	return NF_ACCEPT;
}

static const struct nf_hook_ops millet_pkg_hooks[] = {
	{
		.hook = millet_pkg_ipv4_in,
		.pf = NFPROTO_IPV4,
		.hooknum = NF_INET_LOCAL_IN,
		.priority = MILLET_PKG_PRIORITY,
	},
	{
		.hook = millet_pkg_ipv6_in,
		.pf = NFPROTO_IPV6,
		.hooknum = NF_INET_LOCAL_IN,
		.priority = MILLET_PKG_PRIORITY,
	},
	/*
	 * Stock registers these two no-op output hooks. They are retained because
	 * registration is cheap and exact hook presence may be externally traced
	 * (dash-millet.md, M011).
	 */
	{
		.hook = millet_pkg_out,
		.pf = NFPROTO_IPV4,
		.hooknum = NF_INET_LOCAL_OUT,
		.priority = MILLET_PKG_PRIORITY,
	},
	{
		.hook = millet_pkg_out,
		.pf = NFPROTO_IPV6,
		.hooknum = NF_INET_LOCAL_OUT,
		.priority = MILLET_PKG_PRIORITY,
	},
};

static int __init millet_pkg_init(void)
{
	int ret;

	millet_uid_clear(millet_pkg_uids, ARRAY_SIZE(millet_pkg_uids));
	ret = nf_register_net_hooks(&init_net, millet_pkg_hooks,
				    ARRAY_SIZE(millet_pkg_hooks));
	if (ret)
		return ret;

	ret = register_millet_hook(MILLET_ID_PKG, millet_pkg_recv,
				   millet_pkg_send, millet_pkg_subsystem_init);
	if (ret)
		goto err_hooks;
	ret = init_millet_subsystem(MILLET_ID_PKG);
	if (ret)
		goto err_core;
	return 0;

err_core:
	unregister_millet_hook(MILLET_ID_PKG);
err_hooks:
	nf_unregister_net_hooks(&init_net, millet_pkg_hooks,
				ARRAY_SIZE(millet_pkg_hooks));
	return ret;
}

static void __exit millet_pkg_exit(void)
{
	unregister_millet_hook(MILLET_ID_PKG);
	nf_unregister_net_hooks(&init_net, millet_pkg_hooks,
				ARRAY_SIZE(millet_pkg_hooks));
}

module_init(millet_pkg_init);
module_exit(millet_pkg_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GitHub @YorokobiMaster");
MODULE_DESCRIPTION("Xiaomi Millet one-shot package network reporting");
