// SPDX-License-Identifier: GPL-2.0-only
/* Behavioral reconstruction of dash OS3.0.305 / 6.6.118 minet.ko.
 * This is a stock-behavior replica. Demonstrated defects are intentional;
 * consult STOCK_CONTRACT.md before adding validation or changing lifetime.
 */
#define pr_fmt(fmt) "minet: " fmt

#include <linux/alarmtimer.h>
#include <linux/build_bug.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <net/genetlink.h>
#include <net/inet_sock.h>
#include <net/netfilter/nf_conntrack.h>

#define MINET_CPUS 32
#define MINET_ATTR_MAX 4

/* All wire integers are native little endian in the reference AArch64 ELF. */
struct minet_tuple {
	u8 protocol;
	u8 reserved;
	u16 family;
	u16 peer_port;
	u16 host_port;
	struct in6_addr peer_addr;
	struct in6_addr host_addr;
};

struct minet_counters {
	u32 rx_packets;
	u32 rx_bytes;
	u32 tx_packets;
	u32 tx_bytes;
};

struct minet_flow {
	struct minet_tuple tuple;
	struct minet_counters counters;
	struct list_head entry;
};

struct minet_list {
	u32 count;
	spinlock_t lock;
	struct list_head head;
};

struct minet_app {
	u32 reserved;
	u32 uid;
	struct minet_list flows;
	struct list_head entry;
};

struct minet_context {
	u64 reserved;
	struct task_struct *thread;
	struct minet_list apps[MINET_CPUS];
};

struct minet_app_request {
	u16 version;
	u16 operation;
	u32 count;
	u32 uid[];
};

struct minet_flow_report {
	u16 version;
	u16 reserved0;
	u32 uid;
	u16 count;
	u16 reserved1;
	u8 flows[];
};

struct minet_idletimer {
	struct list_head entry;
	struct alarm unused_alarm;
	struct timer_list timer;
	struct work_struct work;
	char label[28];
	struct timespec64 delayed_timer_trigger;
	struct timespec64 last_modified_timer;
	struct timespec64 last_suspend_time;
	struct notifier_block pm_nb;
	int timeout;
	u32 refcnt;
	u8 timer_type;
	bool work_pending;
	bool send_nl_msg;
	bool active;
	u32 uid;
	bool suspend_time_valid;
};

struct minet_idletimer_info {
	u32 timeout;
	char label[28];
	u8 send_nl_msg;
	u8 timer_type;
	char interface[IFNAMSIZ];
	u8 interface_mask[IFNAMSIZ];
	struct minet_idletimer *timer;
};

struct minet_idle_report {
	char label[28];
	u8 active;
	u8 reserved0[3];
	u64 time_ns;
	u32 uid;
	u32 reserved1;
};

struct minet_tc_app {
	u8 flags;
	u32 uid;
	struct list_head entry;
};

struct minet_tc_request {
	u8 flags;
	u8 operation;
	u16 reserved;
	u32 uid;
};

struct minet_tc_config {
	u32 enabled;
	u32 mask;
	u32 white_mark;
	u32 black_mark;
	u32 user_offset;
	u32 app_start;
};

struct minet_tc_reply {
	u32 enabled;
	u32 status;
	u32 reserved;
};

struct minet_tc_dump {
	struct minet_tc_config config;
	u32 timer_present;
	u32 timeout;
	char label[28];
	char interface[IFNAMSIZ];
	u8 interface_mask[IFNAMSIZ];
};

static_assert(sizeof(struct minet_tuple) == 40);
static_assert(sizeof(struct minet_counters) == 16);
static_assert(sizeof(struct minet_flow) == 72);
static_assert(sizeof(struct minet_app) == 48);
static_assert(sizeof(struct minet_list) == 24);
static_assert(sizeof(struct minet_context) == 784);
static_assert(offsetof(struct minet_flow, entry) == 56);
static_assert(offsetof(struct minet_flow_report, flows) == 12);
static_assert(sizeof(struct minet_idle_report) == 48);
static_assert(sizeof(struct minet_tc_request) == 8);
static_assert(sizeof(struct minet_tc_config) == 24);
static_assert(sizeof(struct minet_tc_reply) == 12);
static_assert(sizeof(struct minet_tc_dump) == 92);
static_assert(offsetof(struct minet_idletimer_info, timer) == 72);

static struct minet_context *g_minet;
static int minet_uid = 1;
module_param(minet_uid, int, 0400);
MODULE_PARM_DESC(minet_uid, "app uid for minet to monitor");

static struct genl_family minet_nl_family;
static struct nla_policy minet_genl_policy[MINET_ATTR_MAX + 1];
static LIST_HEAD(idletimer_tg_list);
static DEFINE_MUTEX(list_mutex);
static DEFINE_SPINLOCK(timestamp_lock);
static struct minet_idletimer_info m_idletimer_info;
static u32 m_tc_enable;
static u32 m_tc_mask = 0x30000000;
static u32 m_white_mark = 0x10000000;
static u32 m_black_mark = 0x20000000;
static u32 m_user_offset = 100000;
static u32 m_app_start = 10000;
static struct {
	u32 count;
	rwlock_t lock;
	struct list_head head;
} m_tc_list;

/* stock .text+0x163c, +0x1808 */
static int minet_nl_msg(const void *data, int len, u32 portid, u8 cmd, u8 attr)
{
	struct sk_buff *skb;
	void *hdr;
	int ret;

	skb = genlmsg_new(nla_total_size(len), GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	hdr = genlmsg_put(skb, portid, 0, &minet_nl_family, 0, cmd);
	if (!hdr)
		goto fail;
	if (nla_put(skb, attr, len, data)) {
		genlmsg_cancel(skb, hdr);
		goto fail;
	}
	genlmsg_end(skb, hdr);
	if (portid)
		ret = netlink_unicast(init_net.genl_sock, skb, portid, MSG_DONTWAIT);
	else
		ret = genlmsg_multicast(&minet_nl_family, skb, 0, 0, GFP_ATOMIC);
	return ret < 0 ? ret : 0;
fail:
	kfree_skb(skb);
	return -EMSGSIZE;
}

static int minet_nl_reply(const void *data, int len, struct genl_info *info,
			  u8 cmd, u8 attr)
{
	struct sk_buff *skb;
	void *hdr;
	int ret;

	skb = genlmsg_new(nla_total_size(len), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	hdr = genlmsg_put(skb, info->snd_portid, info->snd_seq,
			  &minet_nl_family, 0, cmd);
	if (!hdr)
		goto fail;
	if (data && len > 0 && nla_put(skb, attr, len, data)) {
		genlmsg_cancel(skb, hdr);
		goto fail;
	}
	genlmsg_end(skb, hdr);
	ret = netlink_unicast(genl_info_net(info)->genl_sock, skb,
			      info->snd_portid, MSG_DONTWAIT);
	return ret < 0 ? ret : 0;
fail:
	kfree_skb(skb);
	return -EMSGSIZE;
}

static void minet_nl_notify(const void *data, int len)
{
	minet_nl_msg(data, len, 0, 2, 1);
}

static void minet_idletimer_notify(const void *data, int len)
{
	minet_nl_msg(data, len, 0, 4, 1);
}

/* stock .text+0x4, +0xcc, +0x16c, +0x23c */
static struct minet_flow *add_new_flow(struct minet_list *list)
{
	struct minet_flow *flow;

	if (!list) {
		pr_err("%s NULL\n", __func__);
		return NULL;
	}
	flow = kmalloc(sizeof(*flow), GFP_ATOMIC);
	if (flow) {
		memset(flow, 0, sizeof(*flow));
		list_add_tail(&flow->entry, &list->head);
		list->count++;
	}
	return flow;
}

static struct minet_flow *match_flow(struct minet_list *list,
				    const struct minet_tuple *tuple)
{
	struct minet_flow *flow;

	list_for_each_entry(flow, &list->head, entry) {
		if (flow->tuple.family != tuple->family ||
		    flow->tuple.protocol != tuple->protocol ||
		    flow->tuple.peer_port != tuple->peer_port)
			continue;
		if (tuple->family == NFPROTO_IPV4) {
			if (flow->tuple.peer_addr.s6_addr32[0] !=
			    tuple->peer_addr.s6_addr32[0])
				continue;
		} else if (memcmp(&flow->tuple.peer_addr, &tuple->peer_addr, 16)) {
			continue;
		}
		return flow;
	}
	return NULL;
}

static struct minet_app *add_new_app(struct minet_list *list)
{
	struct minet_app *app;

	if (!list) {
		pr_err("%s NULL\n", __func__);
		return NULL;
	}
	app = kmalloc(sizeof(*app), GFP_ATOMIC);
	if (app) {
		memset(app, 0, sizeof(*app));
		INIT_LIST_HEAD(&app->flows.head);
		list_add_tail(&app->entry, &list->head);
		list->count++;
	}
	return app;
}

static struct minet_app *match_app(struct minet_list *list, u32 uid)
{
	struct minet_app *app;

	list_for_each_entry(app, &list->head, entry)
		if (app->uid == uid)
			return app;
	return NULL;
}

/* stock .text+0x270: the second count increments and missing OOM check
 * below are present in the reference instructions (+0x348, +0x4c8).
 */
static int merge_one_app_flow(u32 uid, const struct minet_tuple *tuple,
			      const struct minet_counters *stats,
			      struct minet_list *list)
{
	struct minet_app *app;
	struct minet_flow *flow;

	spin_lock_bh(&list->lock);
	app = match_app(list, uid);
	if (!app) {
		app = add_new_app(list);
		if (app) {
			app->uid = uid;
			list->count++;
		}
	}
	spin_unlock_bh(&list->lock);
	/* Stock dereferences the newly allocated app even after allocation fails. */
	spin_lock_bh(&app->flows.lock);
	flow = match_flow(&app->flows, tuple);
	if (flow) {
		flow->counters.rx_packets += stats->rx_packets;
		flow->counters.rx_bytes += stats->rx_bytes;
		flow->counters.tx_packets += stats->tx_packets;
		flow->counters.tx_bytes += stats->tx_bytes;
	} else {
		flow = add_new_flow(&app->flows);
		if (flow) {
			flow->tuple = *tuple;
			flow->counters = *stats;
			app->flows.count++;
		}
	}
	spin_unlock_bh(&app->flows.lock);
	return 0;
}

static struct minet_list *merge_app_flow(void)
{
	struct minet_list *merged, *list;
	struct minet_app *app;
	struct minet_flow *flow, *next;
	int cpu;

	merged = kmalloc(sizeof(*merged), GFP_KERNEL);
	if (!merged)
		return NULL;
	merged->count = 0;
	spin_lock_init(&merged->lock);
	INIT_LIST_HEAD(&merged->head);
	for (cpu = 0; cpu < MINET_CPUS; cpu++) {
		list = &g_minet->apps[cpu];
		if (list_empty(&list->head))
			continue;
		spin_lock_bh(&list->lock);
		list_for_each_entry(app, &list->head, entry) {
			if (list_empty(&app->flows.head))
				continue;
			spin_lock_bh(&app->flows.lock);
			list_for_each_entry_safe(flow, next, &app->flows.head, entry) {
				list_del_init(&flow->entry);
				app->flows.count--;
				merge_one_app_flow(app->uid, &flow->tuple,
						   &flow->counters, merged);
				kfree(flow);
			}
			spin_unlock_bh(&app->flows.lock);
		}
		spin_unlock_bh(&list->lock);
	}
	return merged;
}

static int minet_notify_app_flow(struct minet_list *list)
{
	struct minet_app *app, *next_app;
	struct minet_flow *flow, *next_flow;
	struct minet_flow_report *report;

	if (!list) {
		pr_err("%s NULL\n", __func__);
		return -1;
	}
	spin_lock_bh(&list->lock);
	list_for_each_entry_safe(app, next_app, &list->head, entry) {
		list_del_init(&app->entry);
		report = kmalloc((size_t)app->flows.count * 56 + 12, GFP_ATOMIC);
		if (report) {
			/* Stock leaves both reserved halfwords uninitialized. */
			report->version = 1;
			report->uid = app->uid;
			report->count = 0;
		} else {
			pr_err("%s malloc failed\n", __func__);
		}
		spin_lock_bh(&app->flows.lock);
		list_for_each_entry_safe(flow, next_flow, &app->flows.head, entry) {
			list_del_init(&flow->entry);
			if (report) {
				memcpy(report->flows + (size_t)report->count * 56,
				       flow, 56);
				report->count++;
			}
			kfree(flow);
		}
		spin_unlock_bh(&app->flows.lock);
		kfree(app);
		if (report) {
			minet_nl_notify(report, (u32)report->count * 56 + 12);
			kfree(report);
		}
	}
	spin_unlock_bh(&list->lock);
	kfree(list);
	return 0;
}

static int add_app(u32 uid)
{
	struct minet_app *app;
	struct minet_list *list;
	int cpu;

	for (cpu = 0; cpu < MINET_CPUS; cpu++) {
		list = &g_minet->apps[cpu];
		spin_lock_bh(&list->lock);
		app = add_new_app(list);
		if (app)
			app->uid = uid;
		spin_unlock_bh(&list->lock);
	}
	return 0;
}

static void destroy_app(struct minet_app *app)
{
	struct minet_flow *flow, *next;

	if (!list_empty(&app->flows.head)) {
		spin_lock_bh(&app->flows.lock);
		list_for_each_entry_safe(flow, next, &app->flows.head, entry) {
			list_del_init(&flow->entry);
			app->flows.count--;
			kfree(flow);
		}
		spin_unlock_bh(&app->flows.lock);
	}
	kfree(app);
}

static int del_app(u32 uid)
{
	struct minet_list *list;
	struct minet_app *app, *next;
	int cpu;

	for (cpu = 0; cpu < MINET_CPUS; cpu++) {
		list = &g_minet->apps[cpu];
		if (list_empty(&list->head))
			continue;
		spin_lock_bh(&list->lock);
		list_for_each_entry_safe(app, next, &list->head, entry) {
			if (app->uid != uid)
				continue;
			list_del_init(&app->entry);
			list->count--;
			destroy_app(app);
		}
		spin_unlock_bh(&list->lock);
	}
	return 0;
}

static int clear_app(void)
{
	struct minet_list *list;
	struct minet_app *app, *next;
	int cpu;

	for (cpu = 0; cpu < MINET_CPUS; cpu++) {
		list = &g_minet->apps[cpu];
		if (list_empty(&list->head))
			continue;
		spin_lock_bh(&list->lock);
		list_for_each_entry_safe(app, next, &list->head, entry) {
			list_del_init(&app->entry);
			list->count--;
			destroy_app(app);
		}
		spin_unlock_bh(&list->lock);
	}
	return 0;
}

static int minet_set_app(const struct minet_app_request *request)
{
	u32 i;

	if (!request || request->version != 1)
		return -1;
	/* Stock trusts the wire count, without checking the attribute length. */
	switch (request->operation) {
	case 1:
		for (i = 0; i < request->count; i++)
			add_app(request->uid[i]);
		break;
	case 2:
		for (i = 0; i < request->count; i++)
			del_app(request->uid[i]);
		break;
	case 3:
		clear_app();
		break;
	}
	return 0;
}

/* stock .text+0x1ee8, +0x2310, +0x2470, +0x2718 */
static void reset_timer(struct minet_idletimer *timer, u32 timeout,
			const u32 *uid)
{
	unsigned long now = jiffies;
	bool previous;

	spin_lock_bh(&timestamp_lock);
	previous = timer->active;
	timer->active = true;
	if (!previous) {
		if (uid)
			timer->uid = *uid;
		if (timer->work_pending) {
			timer->delayed_timer_trigger = timer->last_modified_timer;
		} else {
			timer->work_pending = true;
			schedule_work(&timer->work);
		}
	}
	timer->last_modified_timer = ktime_to_timespec64(ktime_get_boottime());
	mod_timer(&timer->timer, now + msecs_to_jiffies(timeout * 1000));
	spin_unlock_bh(&timestamp_lock);
}

static int mi_idletimer_tg_target_v1(const u32 *uid, struct minet_idletimer *timer)
{
	reset_timer(timer, timer->timeout, uid);
	return 0;
}

static int idletimer_resume(struct notifier_block *nb, unsigned long event,
			    void *unused)
{
	struct minet_idletimer *timer = container_of(nb, struct minet_idletimer, pm_nb);
	struct timespec64 ts;
	unsigned long delta, now = jiffies;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		timer->last_suspend_time = ktime_to_timespec64(ktime_get_boottime());
		timer->suspend_time_valid = true;
		break;
	case PM_POST_SUSPEND:
		if (!timer->suspend_time_valid)
			break;
		timer->suspend_time_valid = false;
		spin_lock_bh(&timestamp_lock);
		if (timer->active && time_after(timer->timer.expires, now)) {
			ts = timespec64_sub(ktime_to_timespec64(ktime_get_boottime()),
					   timer->last_suspend_time);
			delta = timespec64_to_jiffies(&ts);
			if (delta + now < timer->timer.expires) {
				mod_timer_pending(&timer->timer, timer->timer.expires - delta);
			} else {
				timer_delete(&timer->timer);
				timer->timer.expires = 0;
				timer->active = false;
				timer->work_pending = true;
				schedule_work(&timer->work);
			}
		}
		spin_unlock_bh(&timestamp_lock);
		break;
	}
	return NOTIFY_DONE;
}

static void idletimer_tg_work(struct work_struct *work)
{
	struct minet_idletimer *timer = container_of(work, struct minet_idletimer, work);
	struct timespec64 ts, timeout = { timer->timeout, 0 };
	struct minet_idle_report report = {};
	bool state;

	if (!timer->send_nl_msg)
		return;
	ts = ktime_to_timespec64(ktime_get_boottime());
	spin_lock_bh(&timestamp_lock);
	timer->work_pending = false;
	if (timer->timeout < ts.tv_sec - timer->last_modified_timer.tv_sec ||
	    timer->delayed_timer_trigger.tv_sec) {
		state = false;
		if (timer->delayed_timer_trigger.tv_sec) {
			ts = timespec64_add(timer->delayed_timer_trigger, timeout);
			timer->delayed_timer_trigger.tv_sec = 0;
			timer->work_pending = true;
			schedule_work(&timer->work);
		} else {
			ts = timespec64_add(timer->last_modified_timer, timeout);
		}
	} else {
		state = timer->active;
	}
	spin_unlock_bh(&timestamp_lock);
	memcpy(report.label, timer->label, sizeof(report.label));
	report.active = state;
	report.time_ns = timespec64_to_ns(&ts);
	report.uid = timer->uid;
	minet_idletimer_notify(&report, sizeof(report));
}

static void idletimer_tg_expired(struct timer_list *t)
{
	struct minet_idletimer *timer = from_timer(timer, t, timer);

	spin_lock_bh(&timestamp_lock);
	timer->active = false;
	timer->work_pending = true;
	schedule_work(&timer->work);
	spin_unlock_bh(&timestamp_lock);
}

static int mi_idletimer_tg_checkentry_v1(struct minet_idletimer_info *info)
{
	struct minet_idletimer *timer;
	int ret;

	if (!info || !info->timeout || info->timeout > 0x20c49a ||
	    !info->label[0] || strnlen(info->label, sizeof(info->label)) == 28 ||
	    info->send_nl_msg > 1)
		return -EINVAL;
	mutex_lock(&list_mutex);
	list_for_each_entry(timer, &idletimer_tg_list, entry) {
		if (!strcmp(timer->label, info->label)) {
			info->timer = timer;
			timer->refcnt++;
			reset_timer(timer, timer->timeout, NULL);
			mutex_unlock(&list_mutex);
			return 0;
		}
	}
	info->timer = kzalloc(sizeof(*timer), GFP_KERNEL);
	if (!info->timer) {
		mutex_unlock(&list_mutex);
		return -ENOMEM;
	}
	timer = info->timer;
	strncpy(timer->label, info->label, sizeof(timer->label));
	list_add(&timer->entry, &idletimer_tg_list);
	timer->timer_type = 0;
	timer->refcnt = 1;
	/* Stock sets this to true regardless of info->send_nl_msg. */
	timer->send_nl_msg = true;
	timer->active = true;
	timer->timeout = info->timeout;
	timer->last_modified_timer = ktime_to_timespec64(ktime_get_boottime());
	timer->pm_nb.notifier_call = idletimer_resume;
	ret = register_pm_notifier(&timer->pm_nb);
	if (ret)
		pr_err("idletimer_tg_create_v1 register_pm_notifier failed: %d\n", ret);
	INIT_WORK(&timer->work, idletimer_tg_work);
	timer_setup(&timer->timer, idletimer_tg_expired, 0);
	mod_timer(&timer->timer, jiffies + msecs_to_jiffies(timer->timeout * 1000));
	mutex_unlock(&list_mutex);
	return 0;
}

static void mi_idletimer_tg_destroy_v1(struct minet_idletimer *timer)
{
	if (!timer) {
		pr_err("%s NULL\n", __func__);
		return;
	}
	mutex_lock(&list_mutex);
	if (!--timer->refcnt) {
		list_del(&timer->entry);
		timer_shutdown_sync(&timer->timer);
		unregister_pm_notifier(&timer->pm_nb);
		cancel_work_sync(&timer->work);
		kfree(timer);
	}
	mutex_unlock(&list_mutex);
}

/* stock .text+0x2778 and +0x2898 */
static void tl_after_filter_in(struct sk_buff *skb, u32 uid)
{
	struct minet_tc_app *app;
	u8 match = 0;
	int i;

	if (!skb || !m_tc_enable || !skb->dev)
		return;
	for (i = 0; i < IFNAMSIZ; i++)
		if (((u8)skb->dev->name[i] ^ (u8)m_idletimer_info.interface[i]) &
		    m_idletimer_info.interface_mask[i])
			return;
	read_lock_bh(&m_tc_list.lock);
	list_for_each_entry(app, &m_tc_list.head, entry)
		if (app->uid == uid)
			match = app->flags & 1;
	read_unlock_bh(&m_tc_list.lock);
	if (match)
		mi_idletimer_tg_target_v1(&uid, m_idletimer_info.timer);
}

static int add_tc_app(u32 uid, u8 flags)
{
	struct minet_tc_app *app;
	int ret = -1;

	write_lock_bh(&m_tc_list.lock);
	list_for_each_entry(app, &m_tc_list.head, entry) {
		if (app->uid == uid) {
			app->flags |= flags;
			ret = 0;
			goto out;
		}
	}
	if (m_tc_list.count >= 512) {
		pr_err("%s list full\n", __func__);
		goto out;
	}
	app = kmalloc(sizeof(*app), GFP_ATOMIC);
	if (!app) {
		pr_err("%s malloc failed\n", __func__);
		goto out;
	}
	memset(app, 0, sizeof(*app));
	list_add_tail(&app->entry, &m_tc_list.head);
	app->uid = uid;
	app->flags = flags;
	m_tc_list.count++;
	ret = 0;
out:
	write_unlock_bh(&m_tc_list.lock);
	return ret;
}

static void clear_tc_app(void)
{
	struct minet_tc_app *app, *next;

	write_lock_bh(&m_tc_list.lock);
	list_for_each_entry_safe(app, next, &m_tc_list.head, entry) {
		list_del_init(&app->entry);
		kfree(app);
		m_tc_list.count--;
	}
	write_unlock_bh(&m_tc_list.lock);
	pr_info("%s\n", __func__);
}

static int minet_set_tc_list(const struct minet_tc_request *request, int len)
{
	struct minet_tc_app *app;
	int ret = -1;

	if (!request || len != 8 || request->flags >= 16)
		return -1;
	switch (request->operation) {
	case 1:
		return add_tc_app(request->uid, request->flags);
	case 2:
		write_lock_bh(&m_tc_list.lock);
		list_for_each_entry(app, &m_tc_list.head, entry) {
			if (app->uid != request->uid)
				continue;
			app->flags &= ~request->flags;
			if (!app->flags) {
				list_del_init(&app->entry);
				kfree(app);
				m_tc_list.count--;
			}
			ret = 0;
			break;
		}
		write_unlock_bh(&m_tc_list.lock);
		return ret;
	case 3:
		clear_tc_app();
		return 0;
	}
	return -1;
}

static int minet_set_tc_enable(const struct minet_tc_config *config, int len,
			       struct genl_info *info)
{
	struct minet_tc_reply reply = { .enabled = m_tc_enable };

	if (!config || len != 24 || config->enabled > 1) {
		reply.status = 3;
	} else if (!m_tc_enable && config->enabled) {
		m_tc_mask = config->mask;
		m_white_mark = config->white_mark;
		m_black_mark = config->black_mark;
		m_user_offset = config->user_offset;
		m_app_start = config->app_start;
		m_tc_enable = config->enabled;
		reply.enabled = config->enabled;
		mi_idletimer_tg_target_v1(NULL, m_idletimer_info.timer);
		pr_info("%s enable\n", __func__);
	} else if (!config->enabled) {
		if (m_tc_enable) {
			m_tc_enable = 0;
			reply.enabled = config->enabled;
		}
		pr_info("%s disable\n", __func__);
	} else {
		reply.status = 1;
		pr_info("%s already enabled\n", __func__);
	}
	return minet_nl_reply(&reply, sizeof(reply), info, 7, 1);
}

static int print_idletimer_info_safe(const struct minet_idletimer_info *info,
				    char *buf, u32 size)
{
	char local[148] = {};
	int n, i, space;

	if (!info)
		return 0;
	if (!buf || (int)size < 148) {
		buf = local;
		size = sizeof(local);
	}
	n = snprintf(buf, size, "%u ", info->timeout);
	space = min_t(int, size - n, 28);
	n += snprintf(buf + n, space, "%s", info->label);
	n += snprintf(buf + n, (int)(size - n), " ");
	n += snprintf(buf + n, (int)(size - n), "%u ", info->send_nl_msg);
	n += snprintf(buf + n, (int)(size - n), "%u ", info->timer_type);
	space = min_t(int, size - n, 16);
	n += snprintf(buf + n, space, "%s", info->interface);
	n += snprintf(buf + n, (int)(size - n), " {");
	for (i = 0; i < IFNAMSIZ; i++)
		n += snprintf(buf + n, (int)(size - n), "0x%02x ", info->interface_mask[i]);
	n += snprintf(buf + n, (int)(size - n), "}");
	buf[size - 1] = '\0';
	pr_info("idletimer info: %s (%u, %d)\n", buf, size, n);
	return n;
}

static int minet_dump_tc_list(const void *request, int len, struct genl_info *info)
{
	static bool dumping;
	struct minet_tc_dump dump;
	struct minet_tc_app *app;
	u32 parts = 2;
	u8 *list, *cursor;
	size_t total, remaining;

	if (dumping) {
		pr_info("%s already dumping\n", __func__);
		return 0;
	}
	dumping = true;
	dump.config.enabled = m_tc_enable;
	dump.config.mask = m_tc_mask;
	dump.config.white_mark = m_white_mark;
	dump.config.black_mark = m_black_mark;
	dump.config.user_offset = m_user_offset;
	dump.config.app_start = m_app_start;
	dump.timer_present = 1;
	dump.timeout = m_idletimer_info.timeout;
	memcpy(dump.label, m_idletimer_info.label, sizeof(dump.label));
	memcpy(dump.interface, m_idletimer_info.interface, IFNAMSIZ);
	memcpy(dump.interface_mask, m_idletimer_info.interface_mask, IFNAMSIZ);
	print_idletimer_info_safe(&m_idletimer_info, NULL, 0);
	read_lock_bh(&m_tc_list.lock);
	remaining = (size_t)m_tc_list.count * 5;
	total = remaining + 8;
	list = kmalloc(total, GFP_ATOMIC | __GFP_ZERO);
	cursor = NULL;
	if (list) {
		((u32 *)list)[0] = 1;
		((u32 *)list)[1] = m_tc_list.count;
		cursor = list + 8;
		parts = 3;
	} else {
		remaining = 0;
		total = 0;
	}
	list_for_each_entry(app, &m_tc_list.head, entry) {
		pr_info("uid: %u flags: %u\n", app->uid, app->flags);
		if (cursor && remaining >= 4) {
			memcpy(cursor, &app->uid, 4);
			cursor += 4;
			remaining -= 4;
		}
		if (cursor && remaining) {
			*cursor++ = app->flags;
			remaining--;
		}
	}
	read_unlock_bh(&m_tc_list.lock);
	if (minet_nl_reply(&dump, sizeof(dump), info, 7, 3))
		pr_err("%s config reply failed\n", __func__);
	if (list && !remaining && minet_nl_reply(list, total, info, 7, 4))
		pr_err("%s list reply failed\n", __func__);
	if (minet_nl_reply(&parts, sizeof(parts), info, 7, 2))
		pr_err("%s count reply failed\n", __func__);
	dumping = false;
	kfree(list);
	return 0;
}

/* stock uses liberal reparsing with a zero-filled policy table. */
static int minet_parse(struct sk_buff *skb, struct genl_info *info,
			struct nlattr **attrs)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;

	return nla_parse_deprecated(attrs, MINET_ATTR_MAX,
				    (struct nlattr *)(skb->data + 20),
				    nlh->nlmsg_len - 20,
				    minet_genl_policy, info->extack);
}

static int minet_nl_cmd_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[MINET_ATTR_MAX + 1] = {};
	int ret = minet_parse(skb, info, attrs);

	if (ret || !attrs[1])
		return ret;
	return minet_set_app(nla_data(attrs[1]));
}

static int minet_nl_tc_enable_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[MINET_ATTR_MAX + 1] = {};
	int ret = minet_parse(skb, info, attrs);

	if (ret || !attrs[1])
		return ret;
	return minet_set_tc_enable(nla_data(attrs[1]), nla_len(attrs[1]), info);
}

static int minet_nl_tc_list_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[MINET_ATTR_MAX + 1] = {};
	int ret = minet_parse(skb, info, attrs);

	if (ret || !attrs[1])
		return ret;
	return minet_set_tc_list(nla_data(attrs[1]), nla_len(attrs[1]));
}

static int minet_nl_tc_list_dump(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attrs[MINET_ATTR_MAX + 1] = {};
	int ret = minet_parse(skb, info, attrs);

	if (ret || !attrs[1])
		return ret;
	return minet_dump_tc_list(nla_data(attrs[1]), nla_len(attrs[1]), info);
}

static const struct genl_ops minet_nl_ops[] = {
	{ .cmd = 1, .flags = GENL_ADMIN_PERM, .validate = 3, .doit = minet_nl_cmd_set },
	{ .cmd = 3, .flags = GENL_ADMIN_PERM, .validate = 3, .doit = minet_nl_tc_enable_set },
	{ .cmd = 5, .flags = GENL_ADMIN_PERM, .validate = 3, .doit = minet_nl_tc_list_set },
	{ .cmd = 6, .flags = GENL_ADMIN_PERM, .validate = 3, .doit = minet_nl_tc_list_dump },
};

static const struct genl_multicast_group minet_nl_mcgrps[] = {
	{ .name = "minet_event" },
};

static struct genl_family minet_nl_family __ro_after_init = {
	.name = "minet_genl",
	.version = 1,
	.maxattr = MINET_ATTR_MAX,
	.policy = minet_genl_policy,
	.ops = minet_nl_ops,
	.n_ops = ARRAY_SIZE(minet_nl_ops),
	.mcgrps = minet_nl_mcgrps,
	.n_mcgrps = ARRAY_SIZE(minet_nl_mcgrps),
	.resv_start_op = 8,
	.module = THIS_MODULE,
};

/* stock .text+0x12bc: the peer endpoint is the flow key; the first
 * host endpoint survives merging of different sockets to the same peer.
 */
static void app_flow_stat(struct minet_app *app, struct sk_buff *skb,
			  const struct nf_hook_state *state)
{
	struct minet_tuple tuple = {};
	struct minet_flow *flow;
	__be16 *ports = (__be16 *)skb_transport_header(skb);
	bool incoming = state->hook == NF_INET_LOCAL_IN;

	if (state->pf == NFPROTO_IPV6) {
		struct ipv6hdr *ip6 = ipv6_hdr(skb);

		if (ip6 && ports) {
			tuple.protocol = ip6->nexthdr;
			tuple.peer_addr = incoming ? ip6->saddr : ip6->daddr;
			tuple.host_addr = incoming ? ip6->daddr : ip6->saddr;
			tuple.peer_port = ntohs(ports[incoming ? 0 : 1]);
			tuple.host_port = ntohs(ports[incoming ? 1 : 0]);
		}
	} else if (state->pf == NFPROTO_IPV4) {
		struct iphdr *ip4 = ip_hdr(skb);

		if (ip4 && ports) {
			tuple.protocol = ip4->protocol;
			tuple.peer_addr.s6_addr32[0] = incoming ? ip4->saddr : ip4->daddr;
			tuple.host_addr.s6_addr32[0] = incoming ? ip4->daddr : ip4->saddr;
			tuple.peer_port = ntohs(ports[incoming ? 0 : 1]);
			tuple.host_port = ntohs(ports[incoming ? 1 : 0]);
		}
	}
	tuple.family = state->pf;
	spin_lock_bh(&app->flows.lock);
	flow = match_flow(&app->flows, &tuple);
	if (!flow) {
		flow = add_new_flow(&app->flows);
		if (flow)
			flow->tuple = tuple;
	}
	if (flow) {
		if (state->hook == NF_INET_LOCAL_OUT) {
			flow->counters.tx_packets++;
			flow->counters.tx_bytes += skb->len;
		} else if (incoming) {
			flow->counters.rx_packets++;
			flow->counters.rx_bytes += skb->len;
		}
	}
	spin_unlock_bh(&app->flows.lock);
}

static u32 get_sock_uid(const struct sock *sk)
{
	return __kuid_val(sk->sk_uid);
}

static bool minet_uid_for_packet(struct sk_buff *skb,
				 const struct nf_hook_state *state, u32 *uid)
{
	struct sock *sk = skb_to_full_sk(skb);
	struct nf_conn *ct;
	enum ip_conntrack_info ctinfo;

	ct = nf_ct_get(skb, &ctinfo);
	if (sk && sk_fullsock(sk) && (u16)(sk->sk_type - 1) <= 1) {
		*uid = get_sock_uid(sk);
	} else {
		if (!ct || (ctinfo != IP_CT_ESTABLISHED &&
			    ctinfo != IP_CT_ESTABLISHED_REPLY))
			return false;
		*uid = (u32)ct->android_oem_data1;
		if (*uid == U32_MAX)
			return false;
	}
	if (state->hook == NF_INET_LOCAL_OUT && ct && ctinfo == IP_CT_NEW)
		ct->android_oem_data1 = *uid;
	return true;
}

static void minet_account(struct sk_buff *skb, const struct nf_hook_state *state,
			  u32 uid)
{
	struct minet_list *list;
	struct minet_app *app;
	u32 cpu;

	preempt_disable();
	cpu = smp_processor_id();
	if (cpu < MINET_CPUS) {
		list = &g_minet->apps[cpu];
		spin_lock_bh(&list->lock);
		app = match_app(list, uid);
		if (app)
			app_flow_stat(app, skb, state);
		spin_unlock_bh(&list->lock);
	}
	preempt_enable();
}

static bool minet_excluded_v4(__be32 address)
{
	u32 addr = (__force u32)address;

	return (addr & 0xf0ff) == 0x10ac || (addr & 0xff) == 10 ||
		addr == U32_MAX || (addr & 0xff) == 0x7f ||
		(addr & 0xf0) == 0xe0 || (addr & 0xffff) == 0xa8c0 ||
		(addr & 0xffff) == 0xfea9;
}

static unsigned int hook4(void *priv, struct sk_buff *skb,
			  const struct nf_hook_state *state)
{
	struct iphdr *ip = ip_hdr(skb);
	u32 uid;

	if (!ip || (ip->protocol != IPPROTO_TCP && ip->protocol != IPPROTO_UDP))
		return NF_ACCEPT;
	if (!minet_uid_for_packet(skb, state, &uid))
		return NF_ACCEPT;
	if (state->hook == NF_INET_LOCAL_OUT) {
		if (minet_excluded_v4(ip->daddr))
			return NF_ACCEPT;
	} else if (state->hook == NF_INET_LOCAL_IN) {
		if (minet_excluded_v4(ip->saddr))
			return NF_ACCEPT;
		tl_after_filter_in(skb, uid);
	}
	minet_account(skb, state, uid);
	return NF_ACCEPT;
}

static unsigned int hook6(void *priv, struct sk_buff *skb,
			  const struct nf_hook_state *state)
{
	struct ipv6hdr *ip = ipv6_hdr(skb);
	u32 uid;

	if (!ip || (ip->nexthdr != IPPROTO_TCP && ip->nexthdr != IPPROTO_UDP))
		return NF_ACCEPT;
	if (!minet_uid_for_packet(skb, state, &uid))
		return NF_ACCEPT;
	if (state->hook == NF_INET_LOCAL_IN)
		tl_after_filter_in(skb, uid);
	minet_account(skb, state, uid);
	return NF_ACCEPT;
}

static const struct nf_hook_ops minet_hooks[] = {
	{ .hook = hook4, .pf = NFPROTO_IPV4, .hooknum = NF_INET_LOCAL_IN, .priority = 1 },
	{ .hook = hook4, .pf = NFPROTO_IPV4, .hooknum = NF_INET_LOCAL_OUT, .priority = -1 },
	{ .hook = hook6, .pf = NFPROTO_IPV6, .hooknum = NF_INET_LOCAL_IN, .priority = 1 },
	{ .hook = hook6, .pf = NFPROTO_IPV6, .hooknum = NF_INET_LOCAL_OUT, .priority = -1 },
};

static int minet_thread(void *data)
{
	while (!kthread_should_stop()) {
		schedule_timeout_interruptible(1250);
		minet_notify_app_flow(merge_app_flow());
	}
	return 0;
}

static int minet_tc_limiter_init(void)
{
	rwlock_init(&m_tc_list.lock);
	INIT_LIST_HEAD(&m_tc_list.head);
	m_idletimer_info.timeout = 3;
	memcpy(m_idletimer_info.label, "tc_high", 8);
	m_idletimer_info.send_nl_msg = 1;
	memcpy(m_idletimer_info.interface, "wlan0", 6);
	memset(m_idletimer_info.interface_mask, 0xff, 6);
	if (mi_idletimer_tg_checkentry_v1(&m_idletimer_info))
		return -1;
	return 0;
}

static void minet_tc_limiter_finit(void)
{
	clear_tc_app();
	mi_idletimer_tg_destroy_v1(m_idletimer_info.timer);
}

static int minet_nl_init(void)
{
	int ret = genl_register_family(&minet_nl_family);

	if (ret)
		pr_err("%s failed: %d\n", __func__, ret);
	return ret;
}

static void minet_nl_finit(void)
{
	genl_unregister_family(&minet_nl_family);
}

static int __init minet_init_module(void)
{
	struct task_struct *thread;
	int cpu, ret;

	g_minet = kmalloc(sizeof(*g_minet), GFP_KERNEL);
	if (!g_minet)
		return -1;
	memset(g_minet, 0, sizeof(*g_minet));
	for (cpu = 0; cpu < MINET_CPUS; cpu++) {
		spin_lock_init(&g_minet->apps[cpu].lock);
		INIT_LIST_HEAD(&g_minet->apps[cpu].head);
	}
	ret = minet_tc_limiter_init();
	if (ret)
		goto fail;
	ret = nf_register_net_hooks(&init_net, minet_hooks, ARRAY_SIZE(minet_hooks));
	if (ret)
		goto fail;
	thread = kthread_create(minet_thread, g_minet, "minet_thread");
	if (IS_ERR(thread)) {
		g_minet->thread = thread;
		goto fail;
	}
	wake_up_process(thread);
	g_minet->thread = thread;
	if (!thread)
		goto fail;
	ret = minet_nl_init();
	if (!ret)
		return 0;
fail:
	/* Reference .init.text+0xd0: no hook/thread/timer unwind. */
	kfree(g_minet);
	g_minet = NULL;
	return -1;
}

static void __exit minet_exit_module(void)
{
	nf_unregister_net_hooks(&init_net, minet_hooks, ARRAY_SIZE(minet_hooks));
	if (g_minet->thread) {
		kthread_stop(g_minet->thread);
		g_minet->thread = NULL;
	}
	clear_app();
	minet_tc_limiter_finit();
	kfree(g_minet);
	g_minet = NULL;
	minet_nl_finit();
}

module_init(minet_init_module);
module_exit(minet_exit_module);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pengtao He <hepengtao@xiaomi.com>");
MODULE_DESCRIPTION("driver for minet");
MODULE_ALIAS("rtnl-link-minet");
MODULE_VERSION("1.0");
