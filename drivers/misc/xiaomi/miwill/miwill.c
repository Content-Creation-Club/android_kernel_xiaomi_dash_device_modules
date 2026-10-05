/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/ethtool.h>
#include <linux/if_link.h>
#include <linux/random.h>
#include <net/genetlink.h>
#include <net/rtnetlink.h>
#include <net/sch_generic.h>
#include "framework.inc"
#include "redudancy.inc"

static int minet_init(void);
static void minet_cleanup(void);
static int minet_nl_cmd_set(struct sk_buff *, struct genl_info *);

static int miwill_cnt = 1;
static unsigned int management_sequence = 1;
module_param(miwill_cnt, int, 0400);
static struct rtnl_link_ops miwill_link_ops;
static struct genl_family miwill_nl_family;
static void miwill_port_del(struct net_device *, struct net_device *);
static int miwill_port_add_by_name(struct miwill *, const char *, int);
static void miwill_nl_notify_reset(u8);
static void miwill_nl_notify_ip(u32);

static void miwill_mframe(struct miwill *mi, int type)
{
	struct miwill_port *port;
	struct sk_buff *skb;
	u8 *data;
	unsigned int length;
	if (type == 3)
		length = 26;
	else if (type == 1)
		length = 34;
	else {
		pr_err("miwill_mframe: invalid type %d\n", type);
		skb = NULL;
		goto transmit;
	}
	skb = netdev_alloc_skb(mi->dev, length);
	/* Stock dereferences the allocation before testing it. */
	skb_reserve(skb, length);
	data = skb_push(skb, length - 22);
	if (type == 3)
		memcpy(data, mi->address, 4);
	else {
		memcpy(data, mi->local_addr, 6);
		memcpy(data + 6, mi->peer_addr, 6);
	}
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
	data = skb_push(skb, 22);
	data[14] = 0x1f;
	put_unaligned_be16(MIWILL_ETH_P, data + 12);
	data[15] = type;
	put_unaligned_be16(MIWILL_ETH_P, data + 20);
	put_unaligned_be16((u16)management_sequence++, data + 16);
	skb->protocol = htons(MIWILL_ETH_P);
	skb->dev = mi->dev;
transmit:
	list_for_each_entry(port, &mi->ports, list)
		if (port->up && port->index != -1) {
			miwill_dev_queue_xmit(mi, port, skb);
			return;
		}
	kfree_skb(skb);
}
static void miwill_timer(struct timer_list *timer)
{
	struct miwill *mi = from_timer(mi, timer, timer);
	if (++mi->unanswered > 3)
		miwill_nl_notify_reset(1);
	miwill_mframe(mi, 1);
	mod_timer(timer, jiffies + 2500);
}
static int miwill_open(struct net_device *dev)
{
	struct miwill *mi = netdev_priv(dev);
	netif_carrier_on(dev);
	if (!timer_pending(&mi->timer)) {
		mi->timer.expires = jiffies + 500;
		add_timer(&mi->timer);
		mi->unanswered = 0;
	}
	return 0;
}
static int miwill_stop(struct net_device *dev)
{
	struct miwill *mi = netdev_priv(dev);
	struct miwill_port *port, *next;
	miwill_mframe(mi, 3);
	netif_carrier_off(dev);
	list_for_each_entry_safe(port, next, &mi->ports, list)
		miwill_port_del(dev, port->dev);
	if (mi->mode && mi->mode != &dummy_mode && mi->mode->ops->exit)
		mi->mode->ops->exit(mi);
	mi->mode = &dummy_mode;
	timer_delete_sync(&mi->timer);
	return 0;
}
static int miwill_option_enable_get(struct miwill *mi, struct miwill_option_value *value)
{
	return 0;
}
static int miwill_option_enable_set(struct miwill *mi, struct miwill_option_value *value)
{
	int err;
	rtnl_lock();
	if (!value->boolean) {
		miwill_stop(mi->dev);
		rtnl_unlock();
		mi->dev->flags &= ~IFF_UP;
		return 0;
	}
	err = dev_open(mi->dev, NULL);
	rtnl_unlock();
	return err;
}
static int miwill_option_mode_get(struct miwill *mi, struct miwill_option_value *value)
{
	value->ptr = mi->mode->kind;
	return 0;
}
static int miwill_option_mode_set(struct miwill *mi, struct miwill_option_value *value)
{
	return miwill_change_mode(mi, value->ptr);
}
static int miwill_option_gw_addr_get(struct miwill *mi, struct miwill_option_value *value)
{
	return 0;
}
static int miwill_option_gw_addr_set(struct miwill *mi, struct miwill_option_value *value)
{
	memcpy(mi->gateway, value->ptr, 6);
	return 0;
}
static int miwill_option_port_5g_get(struct miwill *mi, struct miwill_option_value *value)
{
	return 0;
}
static int miwill_option_port_24g_get(struct miwill *mi, struct miwill_option_value *value)
{
	return 0;
}
static int miwill_option_port_5g_set(struct miwill *mi, struct miwill_option_value *value)
{
	return miwill_port_add_by_name(mi, value->ptr, 1);
}
static int miwill_option_port_24g_set(struct miwill *mi, struct miwill_option_value *value)
{
	return miwill_port_add_by_name(mi, value->ptr, 2);
}
static const struct miwill_option miwill_options[] = {
	{ .name = "enable", .type = 3, .get = miwill_option_enable_get, .set = miwill_option_enable_set },
	{ .name = "mode", .type = 1, .get = miwill_option_mode_get, .set = miwill_option_mode_set },
	{ .name = "gw_addr", .type = 2, .get = miwill_option_gw_addr_get, .set = miwill_option_gw_addr_set },
	{ .name = "port_5g", .type = 1, .get = miwill_option_port_5g_get, .set = miwill_option_port_5g_set },
	{ .name = "port_24g", .type = 1, .get = miwill_option_port_24g_get, .set = miwill_option_port_24g_set },
};
static int miwill_dev_init(struct net_device *dev)
{
	struct miwill *mi = netdev_priv(dev);
	u32 n, i;
	int err;
	mi->dev = dev;
	spin_lock_init(&mi->lock);
	if (mi->mode && mi->mode != &dummy_mode && mi->mode->ops->exit)
		mi->mode->ops->exit(mi);
	mi->mode = &dummy_mode;
	INIT_LIST_HEAD(&mi->ports);
	INIT_LIST_HEAD(&mi->options);
	INIT_LIST_HEAD(&mi->instances);
	mi->stats = alloc_percpu(struct miwill_stats);
	err = miwill_options_register(mi, miwill_options, ARRAY_SIZE(miwill_options));
	if (err)
		return err;
	if (!mi->stats)
		return -ENOMEM;
	n = dev->num_tx_queues - 1;
	if (n) {
		mi->queues = kmalloc_array(n, sizeof(*mi->queues), GFP_KERNEL);
		if (mi->queues)
			for (i = 0; i < n; ++i)
				INIT_LIST_HEAD(&mi->queues[i]);
	}
	timer_setup(&mi->timer, miwill_timer, 0);
	return 0;
}
static void miwill_dev_uninit(struct net_device *dev)
{
	struct miwill *mi = netdev_priv(dev);
	miwill_options_unregister(mi, miwill_options, ARRAY_SIZE(miwill_options));
	/* Stock frees this native field, not mi->stats. */
	free_percpu(dev->tstats);
}
static netdev_tx_t miwill_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct miwill *mi = netdev_priv(dev);
	struct miwill_port *port;
	unsigned int length = skb->len;
	bool success = false;
	if ((skb->mark & 0xce00000) == 0x8000000) {
		skb = miwill_add_head(skb);
		if (skb) {
			skb->data[15] = skb->data[16] = skb->data[17] = 0;
			skb->data[14] = (skb->data[14] & 0xf0) | 14;
			list_for_each_entry(port, &mi->ports, list)
				if (port->up && port->index != -1) {
					success = miwill_dev_queue_xmit(mi, port, skb) == 0;
					goto account;
				}
			dev_kfree_skb_any(skb);
		}
	} else {
		success = mi->mode->ops->transmit(mi, skb);
	}
account:
	if (success) {
		struct miwill_stats *stats = this_cpu_ptr(mi->stats);
		++stats->tx_packets;
		stats->tx_bytes += length;
	} else {
		this_cpu_inc(mi->stats->tx_dropped);
	}
	return NETDEV_TX_OK;
}
static rx_handler_result_t miwill_handle_frame(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct miwill_port *port;
	struct miwill *mi;
	rx_handler_result_t result;
	u8 *data;
	if (skb_shared(skb)) {
		struct sk_buff *clone = skb_clone(skb, GFP_ATOMIC);
		if (!clone) {
			kfree_skb(skb);
			return RX_HANDLER_CONSUMED;
		}
		consume_skb(skb);
		skb = clone;
	}
	*pskb = skb;
	port = rcu_dereference(skb->dev->rx_handler_data);
	mi = port->miwill;
	if (port->index == -1) {
		this_cpu_inc(mi->stats->rx_nohandler);
		return RX_HANDLER_EXACT;
	}
	data = skb->data;
	if (get_unaligned_be16(data - 2) != MIWILL_ETH_P) {
		this_cpu_inc(mi->stats->rx_nohandler);
		return RX_HANDLER_PASS;
	}
	if ((data[0] & 15) == 14) {
		miwill_remove_head(skb);
		result = RX_HANDLER_ANOTHER;
	} else if ((data[0] & 15) == 15) {
		if (data[1] == 3)
			miwill_nl_notify_reset(2);
		else if (data[1] == 2) {
			mi->unanswered = 0;
			memcpy(mi->address, data + 8, 4);
			miwill_nl_notify_ip(get_unaligned((u32 *)(data + 8)));
		}
		kfree_skb(skb);
		result = RX_HANDLER_CONSUMED;
	} else {
		result = mi->mode->ops->receive(mi, port, skb);
	}
	if (result == RX_HANDLER_ANOTHER) {
		struct miwill_stats *stats = this_cpu_ptr(mi->stats);
		++stats->rx_packets;
		stats->rx_bytes += skb->len;
		if (skb->pkt_type == PACKET_MULTICAST)
			++stats->rx_multicast;
		skb->dev = mi->dev;
		skb->mark |= 0x1000000;
	} else if (result == RX_HANDLER_EXACT || result == RX_HANDLER_PASS) {
		this_cpu_inc(mi->stats->rx_nohandler);
	} else {
		this_cpu_inc(mi->stats->rx_dropped);
	}
	return result;
}
static void miwill_lower_changed(struct miwill_port *port)
{
	struct netdev_lag_lower_state_info state = {
		.link_up = port->up, .tx_enabled = port->index != -1,
	};
	spin_unlock_bh(&port->miwill->lock);
	netdev_lower_state_changed(port->dev, &state);
	spin_lock_bh(&port->miwill->lock);
}
static void __miwill_port_change(struct miwill_port *port, bool up)
{
	struct miwill *mi = port->miwill;
	struct miwill_port *other;
	struct ethtool_link_ksettings settings = {};
	if (port->up != up) {
		port->up = up;
		miwill_lower_changed(port);
	}
	if (!up) {
		port->speed = 0;
		port->duplex = 0;
	} else if (!__ethtool_get_link_ksettings(port->dev, &settings)) {
		port->speed = settings.base.speed;
		port->duplex = settings.base.duplex;
	}
	list_for_each_entry(other, &mi->ports, list)
		if (other->up) {
			netif_carrier_on(mi->dev);
			return;
		}
	netif_carrier_off(mi->dev);
}
static void miwill_port_change(struct miwill_port *port, bool up)
{
	spin_lock_bh(&port->miwill->lock);
	__miwill_port_change(port, up);
	spin_unlock_bh(&port->miwill->lock);
}
static int miwill_device_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct miwill_port *port;
	if (!(dev->priv_flags & IFF_TEAM_PORT))
		return NOTIFY_DONE;
	port = rtnl_dereference(dev->rx_handler_data);
	if (!port)
		return NOTIFY_DONE;
	switch (event) {
	case NETDEV_UP:
	case NETDEV_CHANGE:
		if (dev->operstate == IF_OPER_UP || dev->operstate == IF_OPER_UNKNOWN)
			miwill_port_change(port, true);
		break;
	case NETDEV_DOWN:
		miwill_port_change(port, false);
		break;
	case NETDEV_UNREGISTER:
		miwill_port_change(port, false);
		miwill_port_del(port->miwill->dev, dev);
		break;
	}
	return NOTIFY_DONE;
}
static struct notifier_block miwill_notifier_block = { .notifier_call = miwill_device_event };

#include "ports.inc"

static u16 miwill_select_queue(struct net_device *dev, struct sk_buff *skb, struct net_device *sb_dev)
{
	u16 queue = skb_get_queue_mapping(skb);
	unsigned int selected = queue ? queue - 1 : 0;
	qdisc_skb_cb(skb)->slave_dev_queue_mapping = queue;
	while (selected >= dev->real_num_tx_queues)
		selected -= dev->real_num_tx_queues;
	return selected;
}
static void miwill_change_rx_flags(struct net_device *dev, int flags)
{
	struct miwill *mi = netdev_priv(dev);
	struct miwill_port *port;
	rcu_read_lock();
	list_for_each_entry_rcu(port, &mi->ports, list) {
		if (flags & IFF_PROMISC)
			pr_info("miwill_change_rx_flags: promisc %s\n", dev->name);
		if (flags & IFF_ALLMULTI)
			pr_info("miwill_change_rx_flags: allmulti %s\n", dev->name);
	}
	rcu_read_unlock();
}
static void miwill_set_rx_mode(struct net_device *dev)
{
	struct miwill *mi = netdev_priv(dev);
	struct miwill_port *port;
	rcu_read_lock();
	list_for_each_entry_rcu(port, &mi->ports, list) {
		dev_uc_sync_multiple(port->dev, dev);
		dev_mc_sync_multiple(port->dev, dev);
	}
	rcu_read_unlock();
}
static int miwill_change_mtu(struct net_device *dev, int mtu)
{
	struct miwill *mi = netdev_priv(dev);
	struct miwill_port *port;
	int err;
	spin_lock_bh(&mi->lock);
	list_for_each_entry(port, &mi->ports, list) {
		err = dev_set_mtu(port->dev, mtu);
		if (err)
			goto unwind;
	}
	spin_unlock_bh(&mi->lock);
	dev->mtu = mtu;
	return 0;
unwind:
	list_for_each_entry_continue_reverse(port, &mi->ports, list)
		dev_set_mtu(port->dev, dev->mtu);
	spin_unlock_bh(&mi->lock);
	return err;
}
static void miwill_get_stats64(struct net_device *dev, struct rtnl_link_stats64 *stats)
{
	struct miwill *mi = netdev_priv(dev);
	struct miwill_stats *p;
	u32 rx_dropped = 0, tx_dropped = 0, rx_nohandler = 0;
	int cpu;
	for_each_possible_cpu(cpu) {
		p = per_cpu_ptr(mi->stats, cpu);
		stats->rx_packets += p->rx_packets;
		stats->rx_bytes += p->rx_bytes;
		stats->multicast += p->rx_multicast;
		stats->tx_packets += p->tx_packets;
		stats->tx_bytes += p->tx_bytes;
		rx_dropped += p->rx_dropped;
		tx_dropped += p->tx_dropped;
		rx_nohandler += p->rx_nohandler;
	}
	stats->rx_dropped = rx_dropped;
	stats->tx_dropped = tx_dropped;
	stats->rx_nohandler = rx_nohandler;
}
static netdev_features_t miwill_fix_features(struct net_device *dev, netdev_features_t features)
{
	struct miwill *mi = netdev_priv(dev);
	struct miwill_port *port;
	netdev_features_t result = (features & 0xfffffdf9bfc0fb9eULL) | 0x20000200000ULL;
	rcu_read_lock();
	list_for_each_entry_rcu(port, &mi->ports, list)
		result = netdev_increment_features(result, port->dev->features, features);
	rcu_read_unlock();
	return netdev_increment_features(result, 0x1d0000, features);
}
static int miwill_change_carrier(struct net_device *dev, bool up)
{
	if (up)
		netif_carrier_on(dev);
	else
		netif_carrier_off(dev);
	return 0;
}
static const struct net_device_ops miwill_netdev_ops = {
	.ndo_init = miwill_dev_init, .ndo_uninit = miwill_dev_uninit,
	.ndo_open = miwill_open, .ndo_stop = miwill_stop,
	.ndo_start_xmit = miwill_xmit, .ndo_features_check = passthru_features_check,
	.ndo_select_queue = miwill_select_queue, .ndo_change_rx_flags = miwill_change_rx_flags,
	.ndo_set_rx_mode = miwill_set_rx_mode, .ndo_set_mac_address = eth_mac_addr,
	.ndo_change_mtu = miwill_change_mtu, .ndo_get_stats64 = miwill_get_stats64,
	.ndo_fix_features = miwill_fix_features, .ndo_change_carrier = miwill_change_carrier,
};
static void miwill_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	memcpy(info->driver, "miwill", 7);
	memcpy(info->version, "1.0", 4);
}
static const struct ethtool_ops miwill_ethtool_ops = {
	.get_drvinfo = miwill_get_drvinfo, .get_ts_info = ethtool_op_get_ts_info,
};
static void miwill_setup(struct net_device *dev)
{
	u8 address[6];
	get_random_bytes(address, sizeof(address));
	address[0] = (address[0] & ~3) | 2;
	dev_addr_mod(dev, 0, address, ETH_ALEN);
	dev->addr_assign_type = NET_ADDR_RANDOM;
	ether_setup(dev);
	dev->ethtool_ops = &miwill_ethtool_ops;
	dev->rtnl_link_ops = &miwill_link_ops;
	dev->max_mtu = 65535;
	dev->flags = (dev->flags & 0xffffef7d) | IFF_NOARP;
	dev->needs_free_netdev = true;
	dev->priv_flags = (dev->priv_flags & 0xffffffffffb767dfULL) | 0x489000;
	dev->netdev_ops = &miwill_netdev_ops;
	dev->hw_features |= 0x20fc00000ULL;
	dev->features |= dev->hw_features | 0x4000;
}
static int miwill_validate(struct nlattr *tb[], struct nlattr *data[], struct netlink_ext_ack *extack)
{
	if (tb[IFLA_ADDRESS]) {
		if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN)
			return -EINVAL;
		if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS])))
			return -EADDRNOTAVAIL;
	}
	return 0;
}
static struct rtnl_link_ops miwill_link_ops = {
	.kind = "miwill", .setup = miwill_setup, .validate = miwill_validate,
	/* Stock has priv_size = 0 despite ndo_init using private storage. */
};
static int __init miwill_init_one(void)
{
	struct net_device *dev;
	int err;
	dev = alloc_netdev_mqs(sizeof(struct miwill), "miw_oem%d", NET_NAME_ENUM,
			miwill_setup, 16, 16);
	if (!dev)
		return -ENOMEM;
	rtnl_lock();
	err = register_netdevice(dev);
	rtnl_unlock();
	if (err < 0)
		free_netdev(dev);
	return err;
}

#include "netlink.inc"
#include "minet.inc"

static int __init miwill_init_module(void)
{
	int err, i;
	register_netdevice_notifier(&miwill_notifier_block);
	err = rtnl_link_register(&miwill_link_ops);
	if (err < 0)
		goto unregister_notifier;
	for (i = 0; i < miwill_cnt && !err; ++i)
		err = miwill_init_one();
	if (err < 0)
		goto unregister_link;
	err = miwill_nl_init();
	if (err)
		goto unregister_link;
	err = minet_init();
	if (err)
		goto unregister_link;
	err = miwill_mode_register(&red_mode);
	if (!err)
		return 0;
unregister_link:
	rtnl_link_unregister(&miwill_link_ops);
unregister_notifier:
	unregister_netdevice_notifier(&miwill_notifier_block);
	return err;
}
static void __exit miwill_cleanup_module(void)
{
	minet_cleanup();
	miwill_nl_finit();
	rtnl_link_unregister(&miwill_link_ops);
	unregister_netdevice_notifier(&miwill_notifier_block);
	miwill_mode_unregister(&red_mode);
}
EXPORT_SYMBOL(miwill_options_register);
EXPORT_SYMBOL(miwill_options_unregister);
EXPORT_SYMBOL(miwill_change_mode);
EXPORT_SYMBOL(miwill_mode_register);
EXPORT_SYMBOL(miwill_mode_unregister);
EXPORT_SYMBOL(miwill_modeop_port_enter);
module_init(miwill_init_module);
module_exit(miwill_cleanup_module);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Minjun Xi <ximinjun@xiaomi.com> ");
MODULE_DESCRIPTION("driver for MiWiLL-capable device");
MODULE_ALIAS_RTNL_LINK("miwill");
MODULE_VERSION("1.0");
MODULE_PARM_DESC(miwill_cnt, "Number of miwill devices");
