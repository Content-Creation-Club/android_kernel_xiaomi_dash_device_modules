/* SPDX-License-Identifier: GPL-2.0-only */
static const struct nla_policy miwill_genl_policy[10] = {
	[1] = { .type = NLA_STRING }, [2] = { .type = NLA_U8 },
	[3] = { .type = NLA_NUL_STRING }, [4] = { .type = NLA_STRING },
	[5] = { .type = NLA_STRING }, [6] = { .type = NLA_STRING },
	[7] = { .type = NLA_U32 }, [8] = { .type = NLA_U8 },
};
static int miwill_nl_cmd_options_get(struct sk_buff *skb, struct genl_info *info)
{
	/* Stock prints the bytes following the generic header and sends no reply. */
	pr_info("miwill_nl_cmd_options_get: %s\n", skb->data + 24);
	return 0;
}
static int miwill_nl_cmd_options_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[10] = {};
	struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
	struct net_device *dev;
	int err;
	err = __nla_parse(attrs, 9, (struct nlattr *)(skb->data + 20),
			nlh->nlmsg_len - 20, miwill_genl_policy, 0, info->extack);
	if (err)
		return err;
	if (!attrs[1])
		return -1;
	dev = miwill_name_to_dev(nla_data(attrs[1]));
	if (!dev)
		return -1;
	/* Stock drops the reference before dereferencing dev and never checks kind. */
	dev_put(dev);
	return miwill_options_set(netdev_priv(dev),
		attrs[2] ? nla_get_u8(attrs[2]) : -1,
		attrs[3] ? nla_data(attrs[3]) : NULL,
		attrs[4] ? nla_data(attrs[4]) : NULL,
		attrs[5] ? nla_data(attrs[5]) : NULL,
		attrs[6] ? nla_data(attrs[6]) : NULL);
}
static const struct genl_ops miwill_nl_ops[] = {
	{ .cmd = 1, .flags = GENL_ADMIN_PERM,
	  .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
	  .doit = miwill_nl_cmd_options_set },
	{ .cmd = 2, .flags = GENL_ADMIN_PERM,
	  .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
	  .doit = miwill_nl_cmd_options_get },
	{ .cmd = 4, .flags = GENL_ADMIN_PERM,
	  .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
	  .doit = minet_nl_cmd_set },
};
static const struct genl_multicast_group miwill_nl_mcgrps[] = {
	{ .name = "miwill_event" },
};
static struct genl_family miwill_nl_family = {
	.name = "miwill_genl", .version = 1, .maxattr = 9,
	.policy = miwill_genl_policy, .ops = miwill_nl_ops,
	.n_ops = ARRAY_SIZE(miwill_nl_ops),
	.mcgrps = miwill_nl_mcgrps, .n_mcgrps = ARRAY_SIZE(miwill_nl_mcgrps),
	.resv_start_op = 6, .module = THIS_MODULE,
};
static int miwill_nl_msg(const void *payload, int length, int portid,
		unsigned int command, u8 attribute)
{
	struct sk_buff *skb;
	void *header;
	int err;
	skb = alloc_skb(nla_total_size(length) + 20, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	header = genlmsg_put(skb, portid, 0, &miwill_nl_family, 0, command);
	if (!header)
		goto fail;
	if (nla_put(skb, attribute, length, payload)) {
		genlmsg_cancel(skb, header);
		goto fail;
	}
	genlmsg_end(skb, header);
	if (portid)
		err = netlink_unicast(init_net.genl_sock, skb, portid, MSG_DONTWAIT);
	else {
		BUG_ON(!miwill_nl_family.n_mcgrps);
		NETLINK_CB(skb).dst_group = miwill_nl_family.mcgrp_offset;
		err = netlink_broadcast(init_net.genl_sock, skb, 0,
				miwill_nl_family.mcgrp_offset, GFP_ATOMIC);
	}
	return err < 0 ? err : 0;
fail:
	kfree_skb(skb);
	return -EMSGSIZE;
}
static void miwill_nl_notify_ip(u32 address)
{
	miwill_nl_msg(&address, 4, 0, 3, 7);
}
static void miwill_nl_notify_reset(u8 reason)
{
	miwill_nl_msg(&reason, 1, 0, 3, 8);
}
static int __init miwill_nl_init(void)
{
	return genl_register_family(&miwill_nl_family);
}
static void miwill_nl_finit(void)
{
	genl_unregister_family(&miwill_nl_family);
}
