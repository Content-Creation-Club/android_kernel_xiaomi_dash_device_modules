// SPDX-License-Identifier: GPL-2.0
/* Source reconstruction from xr-qi.ko; retained defects are listed separately. */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/sysctl.h>
#include "qi_provider.h"
#define CREATE_TRACE_POINTS
#include "qi-trace.h"
#include "../perf_actuator/perf_actuator.h"

static unsigned long g_qi_enable;
static DEFINE_SPINLOCK(g_qi_lock);
static ATOMIC_NOTIFIER_HEAD(qi_event_chain);
struct qi_inherit_entry { struct list_head list; u32 reserved; u32 pad; void *cookie; };
struct qi_ioctl { u32 attrs; s32 pid; };
unsigned int sysctl_mutex_qos_opt_spin_time_ns = 100000;
unsigned int sysctl_mutex_normal_opt_spin_time_ns = 60000;
static struct ctl_table_header *mqos_cth;
extern int qi_vh_comm_register(void);
extern int qi_vh_binder_register(void);
extern int qi_vh_mutex_register(void);
extern int qi_vh_rwsem_register(void);
extern int qi_vh_percpu_rwsem_register(void);
extern int qi_vh_futex_register(void);
extern void qi_vh_comm_unregister(void);
extern void qi_vh_binder_unregister(void);
extern void qi_vh_mutex_unregister(void);
extern void qi_vh_rwsem_unregister(void);
extern void qi_vh_percpu_rwsem_unregister(void);
extern void qi_vh_futex_unregister(void);

u32 is_qi_feat_enabled(unsigned long bit)
{
	if (!(READ_ONCE(g_qi_enable) & 1))
		return 0;
	/* Stock masks the word index to two bits, including out-of-object reads. */
	return (((unsigned long *)&g_qi_enable)[(bit >> 6) & 3] >> (bit & 63)) & 1;
}
EXPORT_SYMBOL_GPL(is_qi_feat_enabled);

struct mqos_task *get_alloc_mqos_task(struct task_struct *p)
{
	struct mqos_task *mq;
	int i;
	/* Stock dereferences p before checking its usage. */
	if (!refcount_read(&p->usage))
		return NULL;
	spin_lock(&g_qi_lock);
	mq = (void *)(unsigned long)p->android_oem_data1[2];
	if (!mq) {
		mq = kzalloc(sizeof(*mq), GFP_ATOMIC);
		if (mq) {
			spin_lock_init(&mq->qi.lock);
			for (i = 0; i < 5; i++) {
				INIT_LIST_HEAD(&mq->qi.slots[i].entries);
				spin_lock_init(&mq->qi.slots[i].lock);
			}
		}
		p->android_oem_data1[2] = (unsigned long)mq;
	}
	spin_unlock(&g_qi_lock);
	return mq;
}
EXPORT_SYMBOL_GPL(get_alloc_mqos_task);

struct task_qi *get_alloc_task_qi(struct task_struct *p)
{
	struct mqos_task *mq;
	get_task_struct(p);
	mq = get_alloc_mqos_task(p);
	put_task_struct(p);
	return mq ? &mq->qi : NULL;
}
struct task_qi *get_task_qi(struct task_struct *p)
{
	struct mqos_task *mq;
	if (!p)
		return NULL;
	mq = (void *)(unsigned long)p->android_oem_data1[2];
	return mq ? &mq->qi : NULL;
}
u32 get_static_qos_lvl(struct task_struct *p)
{
	struct task_qi *qi = get_task_qi(p);
	return qi ? qi->static_attrs & 15 : 0;
}
u32 get_dyn_qos_lvl(struct task_struct *p)
{
	struct task_qi *qi = get_task_qi(p);
	return qi ? qi->dynamic_attrs & 15 : 0;
}
u32 get_qos_lvl(struct task_struct *p)
{
	return max(get_static_qos_lvl(p), get_dyn_qos_lvl(p));
}
EXPORT_SYMBOL_GPL(get_qos_lvl);
int get_proc_static_qos_lvl(struct task_struct *p)
{
	struct task_qi *qi;
	if (!p) {
		pr_err("xr-qi: null process task\n");
		return -EINVAL;
	}
	qi = get_task_qi(p->group_leader);
	return qi ? qi->process_attrs & 15 : 0;
}
EXPORT_SYMBOL_GPL(get_proc_static_qos_lvl);
bool is_qos_task(struct task_struct *p)
{
	return p && get_qos_lvl(p) == 3;
}
EXPORT_SYMBOL_GPL(is_qos_task);
bool is_qos_rt_task(struct task_struct *p)
{
	if (p->policy == SCHED_NORMAL || p->policy == SCHED_BATCH)
		return is_qos_task(p);
	return true;
}
int register_qievent_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&qi_event_chain, nb);
}
EXPORT_SYMBOL_GPL(register_qievent_notifier);
int unregister_qievent_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&qi_event_chain, nb);
}
EXPORT_SYMBOL_GPL(unregister_qievent_notifier);
int call_qievent_notifiers(unsigned long event, struct qi_event *data)
{
	return atomic_notifier_call_chain(&qi_event_chain, event, data);
}
void qi_intf_schedinfo_update(struct qi_event *e)
{
	if (!e)
		return;
	e->schedattrs = (e->schedattrs & 0xffff000f) | (((e->flags >> 4) & 0xfff) << 4);
}
EXPORT_SYMBOL_GPL(qi_intf_schedinfo_update);
void qi_intf_schedinfo_reset(struct qi_event *e)
{
	if (!e)
		return;
	e->schedattrs &= 0xffff000f;
}
EXPORT_SYMBOL_GPL(qi_intf_schedinfo_reset);

int do_qos_inherit(struct task_struct *from, struct task_struct *to, void *cookie, u32 kind)
{
	struct task_qi *src = get_task_qi(from), *dst = get_task_qi(to);
	struct qi_inherit_slot *slot;
	struct qi_inherit_entry *entry;
	struct qi_event event;
	u32 attrs;
	kind &= 255;
	if (!dst)
		return -EFAULT;
	BUG_ON(kind > 4);
	attrs = !src ? 0 : (src->static_attrs & 15) == 3 ? src->static_attrs :
		((src->dynamic_attrs & 15) == 3 ? src->dynamic_attrs : 0);
	slot = &dst->slots[kind];
	spin_lock(&slot->lock);
	list_for_each_entry(entry, &slot->entries, list)
		if (entry->cookie == cookie) {
			spin_unlock(&slot->lock);
			return -EEXIST;
		}
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		spin_unlock(&slot->lock);
		return -ENOMEM;
	}
	entry->cookie = cookie;
	list_add(&entry->list, &slot->entries);
	spin_lock(&dst->lock);
	dst->inherit_mask |= BIT(kind);
	if (!dst->inherit_deadline)
		dst->inherit_deadline = jiffies + 500;
	event = (struct qi_event){ from, to, attrs, attrs & 15 };
	call_qievent_notifiers(3, &event);
	dst->dynamic_attrs = event.schedattrs;
	spin_unlock(&dst->lock);
	spin_unlock(&slot->lock);
	return 0;
}
int undo_qos_inherit(struct task_struct *p, struct task_qi *qi, void *cookie, u32 kind)
{
	struct qi_inherit_entry *entry;
	struct qi_inherit_slot *slot;
	struct qi_event event;
	kind &= 255;
	BUG_ON(kind > 4);
	slot = &qi->slots[kind];
	spin_lock(&slot->lock);
	list_for_each_entry(entry, &slot->entries, list) {
		if (entry->cookie != cookie)
			continue;
		/* Stock mistakes the tail node for the only remaining node. */
		if (entry->list.next == &slot->entries) {
			spin_lock(&qi->lock);
			qi->inherit_mask &= ~BIT(kind);
			if (!qi->inherit_mask) {
				qi->inherit_deadline = 0;
				event = (struct qi_event){ NULL, p, 0, qi->dynamic_attrs };
				call_qievent_notifiers(4, &event);
				qi->dynamic_attrs = 0;
			}
			spin_unlock(&qi->lock);
		}
		list_del(&entry->list);
		kfree(entry);
		spin_unlock(&slot->lock);
		return 0;
	}
	spin_unlock(&slot->lock);
	return -1;
}
void qi_free_task_extension(struct task_struct *p)
{
	struct mqos_task *mq = (void *)(unsigned long)p->android_oem_data1[2];
	struct qi_inherit_entry *entry, *tmp;
	int i;
	if (!is_qi_feat_enabled(0) || !mq)
		return;
	if (mq->qi.inherit_deadline && (long)(mq->qi.inherit_deadline - jiffies) < 0)
		(void)jiffies_to_msecs(jiffies - mq->qi.inherit_deadline);
	for (i = 0; i < 5; i++) {
		spin_lock(&mq->qi.slots[i].lock);
		list_for_each_entry_safe(entry, tmp, &mq->qi.slots[i].entries, list) {
			list_del(&entry->list);
			kfree(entry);
		}
		spin_unlock(&mq->qi.slots[i].lock);
	}
	kfree(mq);
	p->android_oem_data1[2] = 0;
}

static int qi_intf_set_qos(void *arg)
{
	struct qi_ioctl req;
	struct task_struct *p, *thread, *leader;
	struct task_qi *qi, *lqi;
	struct qi_event event;
	u32 others = 0, lvl;
	int ret = 0;
	if (!is_qi_feat_enabled(0))
		return -1;
	if (!arg)
		return -EINVAL;
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	lvl = req.attrs & 15;
	if (req.pid < 1 || lvl > 3)
		return -EINVAL;
	rcu_read_lock();
	p = find_task_by_vpid(req.pid);
	if (!p || READ_ONCE(p->__state) == TASK_DEAD) {
		ret = -EINVAL;
		goto out_rcu;
	}
	get_task_struct(p);
	if (get_static_qos_lvl(p) && lvl) {
		ret = -EINVAL;
		goto out_task;
	}
	qi = get_alloc_task_qi(p);
	if (!qi) {
		ret = -ENOMEM;
		goto out_task;
	}
	qi->static_attrs = req.attrs;
	trace_qi_set_qos(p, qi->static_attrs, qi->dynamic_attrs);
	leader = p->group_leader;
	lqi = get_task_qi(leader);
	/* The reference compares the full attribute word with process level. */
	if (lqi && (lqi->process_attrs & 15) > req.attrs)
		for_each_thread(leader, thread)
			others = max(others, get_static_qos_lvl(thread));
	lqi = get_alloc_task_qi(leader);
	if (lqi)
		lqi->process_attrs = max(others, req.attrs);
	else
		pr_err("xr-qi: process qos allocation failed\n");
	event = (struct qi_event){ NULL, p, 0, req.attrs };
	if (lvl == 2 || lvl == 3)
		call_qievent_notifiers(1, &event);
	else if (!lvl)
		call_qievent_notifiers(2, &event);
 out_task:
	rcu_read_unlock();
	put_task_struct(p);
	return ret;
 out_rcu:
	rcu_read_unlock();
	return ret;
}
static int qi_intf_get_qos(void *arg, bool process)
{
	struct qi_ioctl req;
	struct task_struct *p, *thread;
	u32 qos;
	int ret = 0;
	if (!is_qi_feat_enabled(0))
		return -1;
	if (!arg)
		return -EINVAL;
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	rcu_read_lock();
	p = find_task_by_vpid(req.pid);
	if (!p || READ_ONCE(p->__state) == TASK_DEAD || (process && p->exit_signal < 0)) {
		ret = -EINVAL;
		goto out;
	}
	get_task_struct(p);
	qos = get_qos_lvl(p);
	if (process)
		for_each_thread(p->group_leader, thread)
			qos = max(qos, get_qos_lvl(thread));
	put_task_struct(p);
	req.attrs = qos;
 out:
	rcu_read_unlock();
	if (!ret && copy_to_user((void __user *)arg, &req, sizeof(req)))
		ret = -EFAULT;
	return ret;
}
static int qi_intf_get_qos_by_pid(void *arg) { return qi_intf_get_qos(arg, true); }
static int qi_intf_get_qos_by_tid(void *arg) { return qi_intf_get_qos(arg, false); }

static ssize_t inherit_enable_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	/* Labels are recovered from stock rodata; feature numbers are ABI. */
	static const char * const labels[] = { "QI_ENABLE", "QI_BINDER", "QI_MUTEX_JUMP",
		"QI_MUTEX_INHERIT", "QI_RWSEM_JUMP", "QI_RWSEM_INHERIT", "QI_PERCPU_RWSEM_JUMP",
		"QI_PERCPU_RWSEM_INHERIT", "QI_FUTEX_INHERIT", "QI_LOCK_OPT" };
	ssize_t len = 0;
	int i;
	for (i = 0; i < ARRAY_SIZE(labels); i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s:\t%d\n", labels[i],
				 (int)is_qi_feat_enabled(i));
	return len;
}
static ssize_t inherit_enable_store(struct kobject *kobj, struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned long long value;
	int ret = kstrtoull(buf, 0, &value);
	if (ret)
		return ret;
	if (value >= 0x400)
		return -EINVAL;
	g_qi_enable = value;
	return count;
}
static struct kobj_attribute inherit_enable_attr = __ATTR(inherit_enable, 0640,
		inherit_enable_show, inherit_enable_store);
static struct attribute *qi_attrs[] = { &inherit_enable_attr.attr, NULL };
static const struct attribute_group qi_attr_group = { .attrs = qi_attrs };
static struct ctl_table mqos_sysctls[] = {
	{ .procname = "mutex_qos_opt_spin_time_ns", .data = &sysctl_mutex_qos_opt_spin_time_ns,
	  .maxlen = sizeof(unsigned int), .mode = 0644, .proc_handler = proc_douintvec_minmax,
	  .extra1 = SYSCTL_ZERO, .extra2 = SYSCTL_INT_MAX },
	{ .procname = "mutex_normal_opt_spin_time_ns", .data = &sysctl_mutex_normal_opt_spin_time_ns,
	  .maxlen = sizeof(unsigned int), .mode = 0644, .proc_handler = proc_douintvec_minmax,
	  .extra1 = SYSCTL_ZERO, .extra2 = SYSCTL_INT_MAX },
	{ }
};
static int qi_sysctl_register(void)
{
	mqos_cth = register_sysctl("kernel/mi_qos", mqos_sysctls);
	if (!mqos_cth)
		pr_err("xr-qi: register sysctl failed\n");
	return 0;
}
static void qi_sysctl_unregister(void)
{
	if (mqos_cth) {
		unregister_sysctl_table(mqos_cth);
		mqos_cth = NULL;
	}
}
static int __init qi_init(void)
{
	int (* const regs[])(void) = { qi_vh_comm_register, qi_vh_binder_register,
		qi_vh_mutex_register, qi_vh_rwsem_register, qi_vh_percpu_rwsem_register,
		qi_vh_futex_register, qi_sysctl_register };
	void (* const unregs[])(void) = { qi_vh_futex_unregister, qi_vh_percpu_rwsem_unregister,
		qi_vh_rwsem_unregister, qi_vh_mutex_unregister, qi_vh_binder_unregister,
		qi_vh_comm_unregister, qi_sysctl_unregister };
	struct kobject *kobj;
	int i, ret, first = 0;
	kobj = kobject_create_and_add("qos_inherit", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &qi_attr_group);
	if (ret)
		goto out_put;
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regs[i]();
		if (!first && ret < 0)
			first = ret;
	}
	if (first < 0) {
		for (i = 0; i < ARRAY_SIZE(unregs); i++)
			unregs[i]();
		sysfs_remove_group(kobj, &qi_attr_group);
		ret = first;
		goto out_put;
	}
	register_perf_actuator(0x4008781b, qi_intf_set_qos);
	register_perf_actuator(0xc008781c, qi_intf_get_qos_by_pid);
	register_perf_actuator(0xc008781d, qi_intf_get_qos_by_tid);
	return 0;
 out_put:
	kobject_put(kobj);
	return ret;
}
module_init(qi_init);
/* Stock has no cleanup_module, so no module_exit is introduced. */
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Peide Du <dupeide@xiaomi.com>");
MODULE_SOFTDEP("pre: perf-actuator");
MODULE_DESCRIPTION("Xiaomi QOS inherit Driver");
