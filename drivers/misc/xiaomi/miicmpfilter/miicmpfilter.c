// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 GitHub @YorokobiMaster */

#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/skbuff.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>

#include "miicmpfilter_parser.h"

enum miicmpfilter_attr {
	MIICMPFILTER_ATTR_UNSPEC,
	MIICMPFILTER_ATTR_PORT,
	MIICMPFILTER_ATTR_ADDRESS,
	MIICMPFILTER_ATTR_IFNAME,
	__MIICMPFILTER_ATTR_MAX,
};

#define MIICMPFILTER_ATTR_MAX (__MIICMPFILTER_ATTR_MAX - 1)

enum miicmpfilter_cmd {
	MIICMPFILTER_CMD_UNSPEC,
	MIICMPFILTER_CMD_OPEN,
	MIICMPFILTER_CMD_CLOSE,
	MIICMPFILTER_CMD_NOTIFY,
};

enum miicmpfilter_multicast_group {
	MIICMPFILTER_MCGRP_EVENTS,
};

static bool net_registered;
static struct genl_family imcp_unreach_genl_family;

static bool send_nl_msg(u16 port, const char *address, const char *ifname)
{
	struct sk_buff *skb;
	void *header;
	int ret;

	skb = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb)
		return false;

	header = genlmsg_put(skb, 0, 0, &imcp_unreach_genl_family, 0,
			     MIICMPFILTER_CMD_NOTIFY);
	if (!header)
		goto free_skb;

	if (nla_put_u16(skb, MIICMPFILTER_ATTR_PORT, port) ||
	    nla_put(skb, MIICMPFILTER_ATTR_ADDRESS, strlen(address), address) ||
	    nla_put(skb, MIICMPFILTER_ATTR_IFNAME, strlen(ifname), ifname))
		goto free_skb;

	genlmsg_end(skb, header);
	ret = genlmsg_multicast(&imcp_unreach_genl_family, skb, 0,
				MIICMPFILTER_MCGRP_EVENTS, GFP_ATOMIC);
	if (ret < 0)
		pr_err("MIICMPFILTER: Failed to send genl msg: %d\n", ret);

	return true;

free_skb:
	nlmsg_free(skb);
	return false;
}

static unsigned int hook_func(void *priv, struct sk_buff *skb,
			      const struct nf_hook_state *state)
{
	struct miicmpfilter_event event;
	struct iphdr ip_buffer;
	u8 icmp_buffer[MIICMPFILTER_ICMP_BYTES];
	const struct iphdr *ip;
	const u8 *icmp;
	/*
	 * Intentional stock difference (dash-miicmpfilter.md, MF010): stock's
	 * global address buffer can be overwritten by another CPU between
	 * formatting and netlink copying. Keep each notification independent.
	 */
	char address[16];

	if (!skb || !state || !state->out ||
	    !miicmpfilter_is_wlan(state->out->name))
		return NF_ACCEPT;

	/*
	 * Intentional stock difference (docs/driver-bringup/dash-miicmpfilter.md,
	 * MF010): stock dereferences headers through ICMP byte 31 unchecked.
	 * Copy non-linear headers when needed; unavailable bytes suppress only
	 * the notification, never packet delivery (NF_ACCEPT is retained).
	 */
	ip = skb_header_pointer(skb, skb_network_offset(skb), sizeof(ip_buffer),
				&ip_buffer);
	icmp = skb_header_pointer(skb, skb_transport_offset(skb),
				  sizeof(icmp_buffer), icmp_buffer);
	if (!ip || !icmp ||
	    !miicmpfilter_parse_event(ip->protocol, ip->saddr, icmp,
				      sizeof(icmp_buffer), &event))
		return NF_ACCEPT;

	snprintf(address, sizeof(address), "%pI4", &event.address);
	send_nl_msg(event.port, address, state->out->name);
	return NF_ACCEPT;
}

static struct nf_hook_ops miicmpfilter_hook = {
	.hook = hook_func,
	.pf = NFPROTO_IPV4,
	.hooknum = NF_INET_POST_ROUTING,
	.priority = NF_IP_PRI_FIRST,
};

static bool register_net_hook(void)
{
	int ret;

	if (net_registered) {
		pr_info("MIICMPFILTER: net hook is already registered\n");
		return true;
	}

	ret = nf_register_net_hooks(&init_net, &miicmpfilter_hook, 1);
	if (ret) {
		pr_err("MIICMPFILTER: Cannot register netfilter\n");
		return false;
	}

	net_registered = true;
	pr_info("MIICMPFILTER: %s\n", __func__);
	return true;
}

static bool unregister_net_hook(void)
{
	if (net_registered) {
		nf_unregister_net_hooks(&init_net, &miicmpfilter_hook, 1);
		net_registered = false;
		pr_info("MIICMPFILTER: %s\n", __func__);
	} else {
		pr_info("MIICMPFILTER: net hook is already unregistered\n");
	}

	return true;
}

static int icmp_unreach_genl_open(struct sk_buff *skb, struct genl_info *info)
{
	register_net_hook();
	return 0;
}

static int icmp_unreach_genl_close(struct sk_buff *skb, struct genl_info *info)
{
	unregister_net_hook();
	return 0;
}

static const struct nla_policy icmp_unreach_genl_policy[MIICMPFILTER_ATTR_MAX + 1] = {
	[MIICMPFILTER_ATTR_PORT] = { .type = NLA_U16 },
	[MIICMPFILTER_ATTR_ADDRESS] = { .type = NLA_UNSPEC, .len = 16 },
	[MIICMPFILTER_ATTR_IFNAME] = { .type = NLA_UNSPEC, .len = 16 },
};

static const struct genl_ops icmp_unreach_genl_ops[] = {
	{
		.cmd = MIICMPFILTER_CMD_OPEN,
		.flags = GENL_ADMIN_PERM,
		.validate = GENL_DONT_VALIDATE_STRICT |
			    GENL_DONT_VALIDATE_DUMP,
		.doit = icmp_unreach_genl_open,
	},
	{
		.cmd = MIICMPFILTER_CMD_CLOSE,
		.flags = GENL_ADMIN_PERM,
		.validate = GENL_DONT_VALIDATE_STRICT |
			    GENL_DONT_VALIDATE_DUMP,
		.doit = icmp_unreach_genl_close,
	},
};

static const struct genl_multicast_group icmp_unreach_genl_mcgrps[] = {
	[MIICMPFILTER_MCGRP_EVENTS] = { .name = "events" },
};

static struct genl_family imcp_unreach_genl_family = {
	.name = "icmp_unreach",
	.version = 1,
	.maxattr = MIICMPFILTER_ATTR_MAX,
	.resv_start_op = MIICMPFILTER_ATTR_MAX + 1,
	.policy = icmp_unreach_genl_policy,
	.ops = icmp_unreach_genl_ops,
	.n_ops = ARRAY_SIZE(icmp_unreach_genl_ops),
	.mcgrps = icmp_unreach_genl_mcgrps,
	.n_mcgrps = ARRAY_SIZE(icmp_unreach_genl_mcgrps),
	.module = THIS_MODULE,
};

static bool register_genl_family(void)
{
	int ret;

	ret = genl_register_family(&imcp_unreach_genl_family);
	if (ret)
		pr_err("MIICMPFILTER: failed register icmp filter family, err: %d\n",
		       ret);

	return ret == 0;
}

static void unregister_genl_family(void)
{
	genl_unregister_family(&imcp_unreach_genl_family);
}

static int __init miicmpfilter_init(void)
{
	if (!register_genl_family())
		return -1;

	net_registered = false;
	pr_info("MIICMPFILTER: init\n");
	return 0;
}

static void __exit miicmpfilter_exit(void)
{
	unregister_genl_family();
	if (net_registered)
		nf_unregister_net_hooks(&init_net, &miicmpfilter_hook, 1);
	net_registered = false;
	pr_info("MIICMPFILTER: exit\n");
}

module_init(miicmpfilter_init);
module_exit(miicmpfilter_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GitHub @YorokobiMaster");
MODULE_DESCRIPTION("ICMP unreachable module using netfilter hooks");
MODULE_ALIAS("rtnl-link-miicmpfilter");
MODULE_VERSION("1.0");
