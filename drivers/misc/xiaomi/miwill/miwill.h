/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_MIWiLL_H
#define _XIAOMI_MIWiLL_H
#include <linux/bitmap.h>
#include <linux/etherdevice.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/timer.h>

#define MIWILL_ETH_P 0xeee1
#define MIWILL_HLEN 8
struct miwill;
struct miwill_port;
struct miwill_option_value {
	union { const void *ptr; bool boolean; };
	u32 data_len;
	u32 unknown_0c;
	u64 unknown_10;
};
struct miwill_option_info {
	u32 array_index;
	u32 unknown_04;
	struct miwill_port *port;
};
struct miwill_option {
	struct list_head list;
	const char *name;
	bool per_port;
	u8 unknown_19[3];
	u32 array_size;
	u32 type;
	u32 unknown_24;
	int (*init)(struct miwill *, struct miwill_option_info *);
	int (*get)(struct miwill *, struct miwill_option_value *);
	int (*set)(struct miwill *, struct miwill_option_value *);
};
struct miwill_option_instance {
	struct list_head list;
	struct miwill_option *option;
	struct miwill_option_info info;
};
struct miwill_mode_ops {
	int (*init)(struct miwill *);
	void (*exit)(struct miwill *);
	rx_handler_result_t (*receive)(struct miwill *, struct miwill_port *, struct sk_buff *);
	bool (*transmit)(struct miwill *, struct sk_buff *);
	int (*port_enter)(struct miwill *, struct miwill_port *);
	void (*port_leave)(struct miwill *, struct miwill_port *);
	void (*port_enabled)(struct miwill *, struct miwill_port *);
	void (*port_disabled)(struct miwill *, struct miwill_port *);
};
struct miwill_mode {
	const char *kind;
	struct module *owner;
	u64 unknown_10;
	u64 port_priv_size;
	const struct miwill_mode_ops *ops;
	void *priv;
};
struct miwill_stats {
	u64 rx_packets, rx_bytes, rx_multicast, tx_packets, tx_bytes;
	u32 rx_dropped, tx_dropped, rx_nohandler;
	u32 unknown_34;
};
struct miwill_port {
	struct net_device *dev;
	struct hlist_node index_node;
	struct list_head list;
	struct miwill *miwill;
	int index;
	u16 sender_tail;
	u16 unknown_36;
	bool up;
	u8 unknown_39[3];
	u32 speed;
	u8 duplex;
	u8 unknown_41[3];
	u32 original_mtu;
	u32 priority;
	u16 queue_id;
	u16 unknown_4e;
	struct list_head queue_list;
	struct rcu_head rcu;
	u32 band;
	u32 unknown_74;
};
struct miwill {
	struct net_device *dev;
	struct miwill_stats __percpu *stats;
	spinlock_t lock;
	int enabled_ports;
	struct hlist_head index_hash[4];
	struct list_head ports, options, instances;
	struct miwill_mode *mode;
	bool queue_override;
	u8 unknown_71[7];
	struct list_head *queues;
	u8 gateway[6];
	u8 local_addr[6];
	u8 peer_addr[6];
	u8 address[4];
	u8 unanswered;
	u8 unknown_97;
	struct timer_list timer;
};

int miwill_options_register(struct miwill *, const struct miwill_option *, size_t);
void miwill_options_unregister(struct miwill *, const struct miwill_option *, size_t);
int miwill_options_set(struct miwill *, s8, const char *, const u8 *, const char *, const char *);
int miwill_change_mode(struct miwill *, const char *);
int miwill_mode_register(struct miwill_mode *);
void miwill_mode_unregister(struct miwill_mode *);
int miwill_modeop_port_enter(struct miwill *, struct miwill_port *);
struct net_device *miwill_name_to_dev(const char *);
#endif
