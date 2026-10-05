/* SPDX-License-Identifier: GPL-2.0-only */
#include <asm/unaligned.h>
#include <linux/jiffies.h>

struct red_priv {
	u64 sequence;
	unsigned long *bitmap[4];
	u64 generation;
	struct timer_list timer;
};
static DECLARE_BITMAP(dupi_bitmap1, 65536);
static DECLARE_BITMAP(dupi_bitmap2, 65536);
static DECLARE_BITMAP(dupi_bitmap3, 65536);
static DECLARE_BITMAP(dupi_bitmap4, 65536);
static struct red_priv red_priv_data;
static DEFINE_SPINLOCK(bitmap_lock);

static bool is_duplicate(struct miwill *mi, unsigned int sequence)
{
	struct red_priv *priv = mi->mode->priv;
	unsigned int i;
	for (i = 0; i < 4; ++i)
		if (test_and_clear_bit(sequence, priv->bitmap[i]))
			return true;
	set_bit(sequence, priv->bitmap[(-(u32)priv->generation) & 3]);
	return false;
}
static void timer_callback(struct timer_list *timer)
{
	struct red_priv *priv = from_timer(priv, timer, timer);
	spin_lock(&bitmap_lock);
	memset(priv->bitmap[(~(u32)priv->generation) & 3], 0, 8192);
	++priv->generation;
	spin_unlock(&bitmap_lock);
	mod_timer(timer, jiffies + 62);
}
static void timer_init(void)
{
	timer_setup(&red_priv_data.timer, timer_callback, 0);
	red_priv_data.timer.expires = jiffies + 62;
	add_timer(&red_priv_data.timer);
	red_priv_data.generation = 0;
}
static int timer_exit(struct miwill *mi)
{
	struct red_priv *priv = mi->mode->priv;
	return timer_delete(&priv->timer);
}
static int red_init(struct miwill *mi)
{
	red_priv_data.sequence = 0;
	red_priv_data.bitmap[0] = dupi_bitmap1;
	red_priv_data.bitmap[1] = dupi_bitmap2;
	red_priv_data.bitmap[2] = dupi_bitmap3;
	red_priv_data.bitmap[3] = dupi_bitmap4;
	memset(dupi_bitmap1, 0, 8192);
	memset(dupi_bitmap2, 0, 8192);
	memset(dupi_bitmap3, 0, 8192);
	memset(dupi_bitmap4, 0, 8192);
	timer_init();
	pr_info("red_init\n");
	return 0;
}
static void red_exit(struct miwill *mi)
{
	struct red_priv *priv = mi->mode->priv;
	unsigned int i;
	for (i = 0; i < 4; ++i)
		memset(priv->bitmap[i], 0, 8192);
	timer_exit(mi);
	pr_info("red_exit\n");
}
static void miwill_remove_head(struct sk_buff *skb)
{
	u8 *data = skb->data;
	skb->protocol = get_unaligned((__be16 *)(data + 6));
	memmove(data - 6, data - ETH_HLEN, 12);
	skb_pull(skb, MIWILL_HLEN);
	skb->mac_header += MIWILL_HLEN;
	skb->network_header += MIWILL_HLEN;
	skb->transport_header += MIWILL_HLEN;
	skb->mac_len = skb->network_header - skb->mac_header;
}
static rx_handler_result_t red_receive(struct miwill *mi,
		struct miwill_port *port, struct sk_buff *skb)
{
	if (is_duplicate(mi, get_unaligned_be16(skb->data + 2))) {
		consume_skb(skb);
		return RX_HANDLER_CONSUMED;
	}
	miwill_remove_head(skb);
	return RX_HANDLER_ANOTHER;
}
static struct sk_buff *miwill_add_head(struct sk_buff *skb)
{
	int extra = skb_headroom(skb) < 9 ? 8 - skb_headroom(skb) : 0;
	u8 *data;
	__be16 protocol;
	if ((extra || skb_header_cloned(skb)) &&
		pskb_expand_head(skb, (extra + 63) & 64, 0, GFP_ATOMIC)) {
		consume_skb(skb);
		return NULL;
	}
	data = skb_push(skb, MIWILL_HLEN);
	skb_reset_mac_header(skb);
	skb->network_header -= MIWILL_HLEN;
	protocol = get_unaligned((__be16 *)(data + 20));
	memmove(data, data + 8, 12);
	put_unaligned(protocol, (__be16 *)(data + 20));
	put_unaligned_be16(MIWILL_ETH_P, data + 12);
	data[14] = (data[14] & 15) | 0x10;
	skb->protocol = htons(MIWILL_ETH_P);
	skb->priority = 7;
	return skb;
}
static int miwill_dev_queue_xmit(struct miwill *mi,
		struct miwill_port *port, struct sk_buff *skb)
{
	if (!port->dev) {
		pr_err("miwill_dev_queue_xmit: port->dev is NULL\n");
		consume_skb(skb);
		return -1;
	}
	skb->dev = port->dev;
	memcpy(skb->data, mi->gateway, 6);
	memcpy(skb->data + 6, port->dev->dev_addr, 6);
	memcpy(skb->data + 18, mi->dev->dev_addr + 4, 2);
	return dev_queue_xmit(skb);
}
static bool red_transmit(struct miwill *mi, struct sk_buff *skb)
{
	struct red_priv *priv = mi->mode->priv;
	struct miwill_port *port;
	struct sk_buff *copy;
	int err;
	skb = miwill_add_head(skb);
	if (!skb) {
		pr_err("red_transmit: failed to add header\n");
		return false;
	}
	skb->data[15] = 0;
	skb->data[14] = (skb->data[14] & 0xf0) | 1;
	put_unaligned_be16((u16)priv->sequence, skb->data + 16);
	spin_lock_bh(&mi->lock);
	list_for_each_entry(port, &mi->ports, list) {
		if (!port) {
			spin_unlock_bh(&mi->lock);
			dev_kfree_skb_any(skb);
			return false;
		}
		if (!port->up || port->index == -1)
			continue;
		copy = skb_copy(skb, GFP_ATOMIC);
		if (!copy)
			continue;
		err = miwill_dev_queue_xmit(mi, port, copy);
		if (err)
			pr_err("red_transmit: %s error %d\n",
				port->dev ? port->dev->name : "port->dev is NULL", err);
	}
	spin_unlock_bh(&mi->lock);
	++priv->sequence;
	consume_skb(skb);
	return true;
}
static const struct miwill_mode_ops red_mode_ops = {
	.init = red_init, .exit = red_exit, .receive = red_receive,
	.transmit = red_transmit, .port_enter = miwill_modeop_port_enter,
};
static struct miwill_mode red_mode = {
	.kind = "redudancy", .owner = THIS_MODULE,
	.ops = &red_mode_ops, .priv = &red_priv_data,
};
