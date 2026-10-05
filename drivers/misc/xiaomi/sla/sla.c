// SPDX-License-Identifier: GPL-2.0-only
/*
 * dash OS3.0.305 / 6.6.118 sla.ko collector V 3.0 reconstruction.
 * The requested stock semantics, including defects, are retained.
 * Exact evidence and defect inventory: RECONSTRUCTION.txt in this directory.
 */

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/ip.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/proc_fs.h>
#include <linux/rculist.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tcp.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/udp.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#define SLA_RING_BYTES	0x5000
#define SLA_RING_COUNT	1279
#define SLA_RING_DECLARED_COUNT	1280
#define SLA_RING_MAGIC	0x686a7379
#define SLA_PORT_COUNT	127
#define SLA_COMMAND_BYTES	780
#define SLA_HASH_BITS	8

/* .text 0x100/0x124/0x130/0x14c: the exact 16-byte userspace record. */
struct sla_packet {
	u64 timestamp_ms;
	u32 length;
	u16 local_port;
	u8 direction;
	u8 request;
};

/* Stock's emitted bounds check permits 1280 slots, but the allocation and
 * initial count contain only 1279 slots plus this header.
 */
struct sla_ring {
	u32 magic;
	u32 count;
	u32 index;
	u32 reserved;
	struct sla_packet packets[SLA_RING_DECLARED_COUNT];
};

struct sla_config {
	u8 enable;
	u8 rate_on;
	u16 ports[SLA_PORT_COUNT];
	u16 max_size;
	u16 limit_time;
	u8 voip_enable;
	u8 padding[3];
	u32 voip_uid;
};

struct sla_interface {
	struct list_head node;
	struct proc_dir_entry *mmap_file;
	struct proc_dir_entry *stats_file;
	u64 traffic;
	struct sla_ring *ring;
	char name[IFNAMSIZ];
};

struct sla_pending_packet {
	struct hlist_node node;
	u16 local_port;
};

union sla_voip {
	u64 value;
	struct {
		__be32 address;
		__be16 port;
		u16 reserved;
	};
};

static_assert(sizeof(struct sla_packet) == 16);
static_assert(offsetof(struct sla_packet, request) == 15);
static_assert(offsetof(struct sla_ring, packets) == 16);
static_assert(sizeof(struct sla_config) == 268);
static_assert(offsetof(struct sla_config, max_size) == 0x100);
static_assert(offsetof(struct sla_config, voip_uid) == 0x108);
static_assert(sizeof(struct sla_interface) == 64);
static_assert(offsetof(struct sla_interface, name) == 0x30);
static_assert(sizeof(struct sla_pending_packet) == 24);

static spinlock_t stats_spin_lock;
static spinlock_t pkt_spin_lock;
static spinlock_t list_spin_lock;
static LIST_HEAD(sla_iface_list);
static struct hlist_head pkt_list[1 << SLA_HASH_BITS];
static struct sla_config config_data;
static struct proc_dir_entry *sla_dir;
static struct proc_dir_entry *config_file;
static struct proc_dir_entry *sla_voip_file;
static u64 pkt_alloc_num;
static u64 pkt_free_num;
static union sla_voip voip_state;

/* Exact stock UBSAN bounds failures branch to BRK #0x5512. */
static __always_inline void sla_stock_bounds_trap(void)
{
	asm volatile("brk #0x5512");
	unreachable();
}

static struct sla_pending_packet *pkt_find(u16 port)
{
	struct sla_pending_packet *packet;

	hlist_for_each_entry(packet, &pkt_list[hash_32(port, SLA_HASH_BITS)], node)
		if (packet->local_port == port)
			return packet;
	return NULL;
}

static void pkt_delete(struct sla_pending_packet *packet)
{
	spin_lock(&list_spin_lock);
	pkt_free_num++;
	hlist_del_rcu(&packet->node);
	spin_unlock(&list_spin_lock);
	kfree(packet);
}

static void pkt_hlist_deinit(void)
{
	struct sla_pending_packet *packet, *previous;
	struct hlist_node *node;
	unsigned int i;

	/* .text 0x190c: traverse unlocked and delete the previous node. */
	for (i = 0; i < ARRAY_SIZE(pkt_list); i++) {
		previous = NULL;
		for (node = pkt_list[i].first; node;) {
			packet = hlist_entry(node, struct sla_pending_packet, node);
			if (previous)
				pkt_delete(previous);
			node = packet->node.next;
			previous = packet;
		}
		if (previous)
			pkt_delete(previous);
	}
}

static void pkt_hlist_print(void)
{
	struct sla_pending_packet *packet;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pkt_list); i++)
		hlist_for_each_entry(packet, &pkt_list[i], node)
			printk("%s() pkt_list[%d] local_port=%d\n", __func__, i,
				packet->local_port);
}

static struct sla_interface *sla_find_interface(const char *name)
{
	struct sla_interface *iface;

	list_for_each_entry(iface, &sla_iface_list, node)
		if (!strcmp(iface->name, name))
			return iface;
	return NULL;
}

/* .text 0x5c8: payload TCP; FIN/RST clears a pending port only without payload.
 * The configurable ports array is not read by either packet hook.
 */
static bool sla_pkt_valid(struct sk_buff *skb, u8 direction)
{
	struct iphdr *ip = ip_hdr(skb);
	struct tcphdr *tcp;
	struct sla_pending_packet *packet;
	unsigned long flags;
	u16 port;

	if (ip->protocol != IPPROTO_TCP)
		return false;
	tcp = tcp_hdr(skb);
	port = ntohs(direction == 1 ? tcp->source : tcp->dest);
	if ((u16)(tcp->doff * 4 + ip->ihl * 4) != ntohs(ip->tot_len))
		return true;
	if (tcp->fin || tcp->rst) {
		spin_lock_irqsave(&pkt_spin_lock, flags);
		packet = pkt_find(port);
		if (packet)
			pkt_delete(packet);
		spin_unlock_irqrestore(&pkt_spin_lock, flags);
	}
	return false;
}

static u64 sla_timestamp_ms(void)
{
	struct timespec64 now;

	ktime_get_real_ts64(&now);
	return now.tv_sec * 1000 + now.tv_nsec / NSEC_PER_MSEC;
}

static unsigned int sla_ipv4_in(void *priv, struct sk_buff *skb,
				const struct nf_hook_state *state)
{
	struct sla_interface *iface;
	struct sla_ring *ring;
	struct sla_packet *record, *previous;
	struct sla_pending_packet *pending;
	unsigned long stats_flags, pkt_flags;
	u32 index, previous_index, length;

	if (!config_data.enable || !config_data.rate_on || !sla_pkt_valid(skb, 2))
		return NF_ACCEPT;
	spin_lock_irqsave(&stats_spin_lock, stats_flags);
	iface = sla_find_interface(state->in->name);
	if (!iface)
		goto unlock_stats;
	iface->traffic += skb->len;
	ring = iface->ring;
	index = ring->index;
	if (index >= SLA_RING_DECLARED_COUNT)
		sla_stock_bounds_trap();
	record = &ring->packets[index];
	record->direction = 2;
	record->timestamp_ms = sla_timestamp_ms();
	record->length = skb->len + ETH_HLEN;
	record->local_port = ntohs(tcp_hdr(skb)->dest);
	spin_lock_irqsave(&pkt_spin_lock, pkt_flags);
	pending = pkt_find(record->local_port);
	if (pending) {
		pkt_delete(pending);
		record->request = 1;
	} else {
		record->request = 0;
		index = ring->index;
		if (index >= SLA_RING_DECLARED_COUNT)
			sla_stock_bounds_trap();
		previous_index = (index ? index : ring->count) - 1;
		if (previous_index >= SLA_RING_COUNT)
			sla_stock_bounds_trap();
		previous = &ring->packets[previous_index];
		if (!previous->request &&
		    previous->local_port == ring->packets[index].local_port &&
		    previous->timestamp_ms + config_data.limit_time >=
			    ring->packets[index].timestamp_ms) {
			length = previous->length + ring->packets[index].length;
			if (length <= config_data.max_size) {
				previous->length = length;
				spin_unlock_irqrestore(&pkt_spin_lock, pkt_flags);
				memset(record, 0, sizeof(*record));
				goto unlock_stats;
			}
		}
	}
	spin_unlock_irqrestore(&pkt_spin_lock, pkt_flags);
	ring->index = ring->index + 1 < ring->count ? ring->index + 1 : 0;
unlock_stats:
	spin_unlock_irqrestore(&stats_spin_lock, stats_flags);
	return NF_ACCEPT;
}

static unsigned int sla_ipv4_out(void *priv, struct sk_buff *skb,
				 const struct nf_hook_state *state)
{
	struct sla_interface *iface;
	struct sla_ring *ring;
	struct sla_packet *record;
	struct sla_pending_packet *pending;
	struct iphdr *ip;
	unsigned long flags;
	u64 timestamp;
	u16 port;
	u32 index;

	if (config_data.enable && config_data.voip_enable && config_data.voip_uid) {
		ip = ip_hdr(skb);
		if (ip->protocol == IPPROTO_UDP && !IS_ERR_OR_NULL(skb->sk)) {
			if (config_data.voip_uid == __kuid_val(skb->sk->sk_uid) &&
			    !voip_state.address && udp_hdr(skb)->dest != htons(53)) {
				voip_state.port = udp_hdr(skb)->dest;
				voip_state.address = ip->daddr;
			}
			return NF_ACCEPT;
		}
	}
	if (!config_data.enable || !config_data.rate_on || !sla_pkt_valid(skb, 1))
		return NF_ACCEPT;
	spin_lock_irqsave(&stats_spin_lock, flags);
	iface = sla_find_interface(state->out->name);
	if (!iface)
		goto unlock;
	ring = iface->ring;
	timestamp = sla_timestamp_ms();
	port = ntohs(tcp_hdr(skb)->source);
	pending = pkt_find(port);
	if (!pending) {
		pending = kzalloc(sizeof(*pending), GFP_ATOMIC);
		if (pending) {
			pending->local_port = port;
			spin_lock(&list_spin_lock);
			pkt_alloc_num++;
			hlist_add_head_rcu(&pending->node,
					   &pkt_list[hash_32(port, SLA_HASH_BITS)]);
			spin_unlock(&list_spin_lock);
		}
	}
	index = ring->index;
	if (index >= SLA_RING_DECLARED_COUNT)
		sla_stock_bounds_trap();
	record = &ring->packets[index];
	record->direction = 1;
	record->timestamp_ms = timestamp;
	record->length = skb->len + ETH_HLEN;
	record->request = 1;
	ring->index = index + 1 < ring->count ? index + 1 : 0;
	record->local_port = ntohs(tcp_hdr(skb)->source);
unlock:
	spin_unlock_irqrestore(&stats_spin_lock, flags);
	return NF_ACCEPT;
}

/* Exact .data..read_mostly constants and .rela.data..read_mostly callbacks. */
static struct nf_hook_ops sla_ipv4_ops[] = {
	{
		.hook = sla_ipv4_in,
		.pf = NFPROTO_IPV4,
		.hooknum = NF_INET_PRE_ROUTING,
		.priority = NF_IP_PRI_CONNTRACK + 2,
	},
	{
		.hook = sla_ipv4_out,
		.pf = NFPROTO_IPV4,
		.hooknum = NF_INET_POST_ROUTING,
		.priority = NF_IP_PRI_LAST,
	},
};

static int stats_open(struct inode *inode, struct file *file)
{
	return simple_open(inode, file);
}

static ssize_t stats_read(struct file *file, char __user *buf, size_t count,
			  loff_t *pos)
{
	return simple_read_from_buffer(buf, count, pos, file->private_data, SLA_RING_BYTES);
}

static int stats_map(struct file *file, struct vm_area_struct *vma)
{
	int ret;

	if (!file->private_data)
		return -1;
	/* Stock .text 0x1aa4: size is arbitrary; pgoff is ignored; PAGE_SHARED. */
	ret = remap_pfn_range(vma, vma->vm_start,
			      virt_to_phys(file->private_data) >> PAGE_SHIFT,
			      vma->vm_end - vma->vm_start, PAGE_SHARED);
	return ret ? -1 : 0;
}

static const struct proc_ops stats_fops = {
	.proc_open = stats_open,
	.proc_read = stats_read,
	.proc_lseek = default_llseek,
	.proc_mmap = stats_map,
};

static int procfs_u64_get(void *data, u64 *value)
{
	*value = *(u64 *)data;
	return 0;
}

static int procfs_u64_set(void *data, u64 value)
{
	*(u64 *)data = value;
	return 0;
}

static int fops_u64_open(struct inode *inode, struct file *file)
{
	return simple_attr_open(inode, file, procfs_u64_get, procfs_u64_set, "%llu\n");
}

static const struct proc_ops fops_u64 = {
	.proc_open = fops_u64_open,
	.proc_read = simple_attr_read,
	.proc_write = simple_attr_write,
	.proc_lseek = generic_file_llseek,
	.proc_release = simple_attr_release,
};

static void sla_interface_start(const char *name)
{
	struct sla_interface *iface;
	char mmap_name[21] = {};
	char stats_name[22] = {};
	unsigned long flags;
	unsigned int i;

	if (!name) {
		pr_crit("### %s Cannot create iface struct when iface name is NULL \n", __func__);
		return;
	}
	spin_lock_irqsave(&stats_spin_lock, flags);
	iface = sla_find_interface(name);
	spin_unlock_irqrestore(&stats_spin_lock, flags);
	if (iface) {
		pr_crit("### %s Interface %s already being tracked in SLA ", __func__, name);
		return;
	}
	iface = kmalloc(sizeof(*iface), GFP_ATOMIC);
	if (!iface) {
		pr_crit("### %s kmalloc Failed for %s", __func__, name);
		return;
	}
	strscpy(iface->name, name, strnlen(name, IFNAMSIZ) + 1);
	pr_crit("### %s copy the iface name into struct %s \n", __func__, name);
	strscpy(mmap_name, iface->name, sizeof(mmap_name));
	strlcat(mmap_name, "_mmap", sizeof(mmap_name));
	strscpy(stats_name, iface->name, sizeof(stats_name));
	strlcat(stats_name, "_stats", sizeof(stats_name));
	iface->ring = kmalloc(SLA_RING_BYTES, GFP_ATOMIC);
	if (!iface->ring) {
		pr_crit("### %s kmalloc wlan_stats Fail\n", __func__);
		goto free_iface;
	}
	memset((char *)iface->ring + 16, 0, SLA_RING_BYTES - 16);
	iface->ring->magic = SLA_RING_MAGIC;
	iface->ring->count = SLA_RING_COUNT;
	iface->ring->index = 0;
	iface->ring->reserved = 0;
	for (i = 0; i < SLA_RING_BYTES; i += PAGE_SIZE)
		SetPageReserved(virt_to_page((char *)iface->ring + i));
	iface->mmap_file = proc_create_data(mmap_name, 0644, sla_dir, &stats_fops, iface->ring);
	if (!iface->mmap_file) {
		pr_err("#### %s Fail to create file: mmap_file for interface= %s \n", __func__, name);
		goto free_ring;
	}
	iface->traffic = 0;
	iface->stats_file = proc_create_data(stats_name, 0644, sla_dir, &fops_u64, &iface->traffic);
	if (!iface->stats_file) {
		pr_err("#### %s Fail to create total_stats_fils for interface = %s \n", __func__, name);
		proc_remove(iface->mmap_file);
		goto free_ring;
	}
	spin_lock_irqsave(&stats_spin_lock, flags);
	list_add(&iface->node, &sla_iface_list);
	spin_unlock_irqrestore(&stats_spin_lock, flags);
	pr_crit("### %s was able to add  %s  %p \n", __func__, name, iface);
	return;
free_ring:
	/* Stock's proc creation failure frees pages without clearing Reserved. */
	kfree(iface->ring);
free_iface:
	kfree(iface);
}

static void sla_stats_disable(void)
{
	struct sla_interface *iface, *next;
	unsigned long flags;
	unsigned int i;

	if (!config_data.enable) {
		printk("### %s disable already\n", __func__);
		return;
	}
	config_data.enable = 0;
	config_data.rate_on = 0;
	spin_lock_irqsave(&stats_spin_lock, flags);
	list_for_each_entry_safe(iface, next, &sla_iface_list, node) {
		pr_crit("### %s trying to disable %s\n", __func__, iface->name);
		iface->traffic = 0;
		if (iface->ring) {
			memset(iface->ring, 0, SLA_RING_BYTES);
			for (i = 0; i < SLA_RING_BYTES; i += PAGE_SIZE)
				ClearPageReserved(virt_to_page((char *)iface->ring + i));
			kfree(iface->ring);
			iface->ring = NULL;
		}
		if (iface->mmap_file)
			proc_remove(iface->mmap_file);
		if (iface->stats_file)
			proc_remove(iface->stats_file);
		pr_crit("### %s removing form list %s\n", __func__, iface->name);
		list_del(&iface->node);
		pr_err("### %s removed form list %s %p \n", __func__, iface->name, iface);
		kfree(iface);
	}
	spin_unlock_irqrestore(&stats_spin_lock, flags);
	pr_crit("### %s out of disable funk \n", __func__);
	pkt_hlist_deinit();
}

static int config_file_show(struct seq_file *seq, void *unused)
{
	struct sla_config *config = seq->private;
	unsigned int i;

	seq_printf(seq, "enable=%d\n", config->enable);
	seq_printf(seq, "rate_on=%d\n", config->rate_on);
	seq_puts(seq, "ports=");
	for (i = 0; i < SLA_PORT_COUNT && config->ports[i]; i++)
		seq_printf(seq, i ? ",%d" : "%d", config->ports[i]);
	seq_puts(seq, "\n");
	seq_printf(seq, "max_size=%d\n", config->max_size);
	seq_printf(seq, "limit_time=%d\n", config->limit_time);
	seq_printf(seq, "voip_enable=%d\n", config->voip_enable);
	seq_printf(seq, "voip_uid=%u\n", config->voip_uid);
	return 0;
}

static int config_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, config_file_show, pde_data(inode));
}

static ssize_t config_file_write(struct file *file, const char __user *input,
				 size_t count, loff_t *pos)
{
	char command[SLA_COMMAND_BYTES] = {};
	char *cursor, *token;
	struct sla_interface *iface;
	unsigned long flags, value;
	unsigned int i;

	if (count > SLA_COMMAND_BYTES || copy_from_user(command, input, count))
		return -EFAULT;
	/* .text 0x850-0x86c: zero count traps; final byte is always discarded. */
	if (!count)
		sla_stock_bounds_trap();
	command[count - 1] = '\0';
	if (!strncmp(command, "iface=", 6)) {
		printk("#### %s CMD_IFACE_ADD=[%s]\n", __func__, command + 6);
		if (strlen(command + 6) < IFNAMSIZ)
			sla_interface_start(command + 6);
		else
			pr_err("#### %s ifname=[%s] is invaild\n", __func__, command + 6);
	} else if (!strncmp(command, "enable=", 7)) {
		printk("#### %s enable=%c\n", __func__, command[7]);
		if (!strcmp(command + 7, "1")) {
			if (!config_data.enable) {
				memset(pkt_list, 0, sizeof(pkt_list));
				config_data.enable = 1;
				config_data.rate_on = 1;
			} else {
				printk("### %s enable already\n", "sla_stats_enable");
			}
		} else if (!strcmp(command + 7, "0")) {
			sla_stats_disable();
		}
	} else if (!strncmp(command, "rate_on=", 8)) {
		printk("#### %s rate_on=%c\n", __func__, command[8]);
		if (!strcmp(command + 8, "1"))
			config_data.rate_on = 1;
		else if (!strcmp(command + 8, "0"))
			config_data.rate_on = 0;
	} else if (!strncmp(command, "ports=", 6)) {
		printk("#### %s ports=[%s]\n", __func__, command + 6);
		cursor = command + 6;
		i = 0;
		while ((token = strsep(&cursor, ","))) {
			value = simple_strtoul(token, NULL, 10);
			if (!value || value > U16_MAX)
				continue;
			config_data.ports[i++] = value;
			printk("#### %s ports=[%d]\n", __func__, (u16)value);
			if (i == SLA_PORT_COUNT)
				break;
		}
	} else if (!strcmp(command, "reset_traffic=1")) {
		spin_lock_irqsave(&stats_spin_lock, flags);
		list_for_each_entry(iface, &sla_iface_list, node)
			iface->traffic = 0;
		spin_unlock_irqrestore(&stats_spin_lock, flags);
		printk("#### %s reset_traffic=%c\n", __func__, command[14]);
	} else if (!strncmp(command, "hash_print", 10)) {
		printk("#### %s hash print %llu, %llu\n", __func__, pkt_alloc_num, pkt_free_num);
		pkt_hlist_print();
	} else if (!strncmp(command, "hash_clear", 10)) {
		printk("#### %s hash clear\n", __func__);
		pkt_hlist_deinit();
	} else if (!strncmp(command, "max_size=", 9)) {
		value = simple_strtoul(command + 9, NULL, 10);
		if (value >= 4096) {
			config_data.max_size = value;
			printk("#### %s max_size=%d\n", __func__, config_data.max_size);
		} else {
			pr_err("#### %s max_size=%lu is invaild, need greater than %d\n",
			       __func__, value, 4096);
		}
	} else if (!strncmp(command, "limit_time=", 11)) {
		value = simple_strtoul(command + 11, NULL, 10);
		if (value >= 5) {
			config_data.limit_time = value;
			printk("#### %s limit_time=%d\n", __func__, config_data.limit_time);
		} else {
			pr_err("#### %s limit_time=%lu is invaild, need longer than %d\n",
			       __func__, value, 5);
		}
	} else if (!strncmp(command, "voip_enable=", 12)) {
		printk("#### %s voip_enbale=%s\n", __func__, command + 12);
		if (!strcmp(command + 12, "1")) {
			voip_state.value = 0;
			if (!sla_voip_file) {
				pr_err("#### %s Fail to open file: sla_voip_file\n", __func__);
				return 1;
			}
			config_data.voip_enable = 1;
		} else if (!strcmp(command + 12, "0")) {
			config_data.voip_enable = 0;
		}
	} else if (!strncmp(command, "voip_uid=", 9)) {
		printk("#### %s voip_uid=%s\n", __func__, command + 9);
		config_data.voip_uid = simple_strtoul(command + 9, NULL, 10);
	}
	return count;
}

static const struct proc_ops config_fops = {
	.proc_open = config_file_open,
	.proc_read = seq_read,
	.proc_write = config_file_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int sla_voip_file_show(struct seq_file *seq, void *unused)
{
	seq_printf(seq, "address=%u\n", (__force u32)voip_state.address);
	seq_printf(seq, "port=%d\n", (__force u16)voip_state.port);
	return 0;
}

static int sla_voip_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, sla_voip_file_show, pde_data(inode));
}

static ssize_t sla_voip_file_read(struct file *file, char __user *buf, size_t count,
				loff_t *pos)
{
	char text[21] = {};

	sprintf(text, "%u,%d", (__force u32)voip_state.address, (__force u16)voip_state.port);
	printk("sla voip file read is %s", text);
	voip_state.value = 0;
	return simple_read_from_buffer(buf, count, pos, text, strlen(text));
}

/* .rela.rodata 0x68/0x70: single_open + custom read, no release or lseek. */
static const struct proc_ops sla_voip_fops = {
	.proc_open = sla_voip_file_open,
	.proc_read = sla_voip_file_read,
};

static int __init sla_static_collector_init(void)
{
	int ret;

	printk("#### %s in version V 3.0 \n", __func__);
	ret = nf_register_net_hooks(&init_net, sla_ipv4_ops, ARRAY_SIZE(sla_ipv4_ops));
	if (ret < 0) {
		pr_err("%s: can't register hooks.\n", __func__);
		return ret;
	}
	spin_lock_init(&list_spin_lock);
	spin_lock_init(&stats_spin_lock);
	spin_lock_init(&pkt_spin_lock);
	sla_dir = proc_mkdir("sla", NULL);
	if (!sla_dir) {
		pr_err("#### %s Fail to create sla dir\n", __func__);
		return 1;
	}
	memset(&config_data, 0, sizeof(config_data));
	config_data.ports[0] = 80;
	config_data.ports[1] = 443;
	config_data.max_size = 4096;
	config_data.limit_time = 5;
	config_file = proc_create_data("config", 0644, sla_dir, &config_fops, &config_data);
	if (!config_file) {
		pr_err("#### %s Fail to create file: config_file\n", __func__);
		return 1;
	}
	voip_state.value = 0;
	sla_voip_file = proc_create_data("sla_voip_state", 0644, sla_dir, &sla_voip_fops, &voip_state);
	if (!sla_voip_file) {
		pr_err("#### %s Fail to create file: sla_voip_file\n", __func__);
		return 1;
	}
	return 0;
}

static void __exit sla_static_collector_deinit(void)
{
	printk("#### sla_static_collector_deinit in\n");
	nf_unregister_net_hooks(&init_net, sla_ipv4_ops, ARRAY_SIZE(sla_ipv4_ops));
	sla_stats_disable();
	proc_remove(sla_dir);
}

module_init(sla_static_collector_init);
module_exit(sla_static_collector_deinit);
MODULE_LICENSE("GPL v2");
