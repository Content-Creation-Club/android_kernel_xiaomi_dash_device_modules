// SPDX-License-Identifier: GPL-2.0-only
/* dash OS3.0.305 xr-qs reconstruction. See RECONSTRUCTION.txt before enabling. */
#include <linux/bitops.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sysfs.h>
#include <uapi/linux/sched/types.h>
#include <kernel/sched/sched.h>
#include <binder_internal.h>
#include <trace/hooks/binder.h>
#include <trace/hooks/dtask.h>
#include <trace/hooks/sched.h>
#include "../mi_schedule/mi_schedule.h"
#include "../xr_qi/qi_provider.h"

#define CREATE_TRACE_POINTS
#include "qs_trace.h"

static unsigned int g_qs_enable;
static unsigned int g_sl_enable;
static unsigned int g_surfaceflinger_prio = 97;
static unsigned int g_sl_prio = 99;

struct qs_config {
	u8 sched_rr;
	u8 inherit_prio_fork;
	u8 pad[2];
	u32 prio;
};

bool is_qs_enabled(void)
{
	return g_qs_enable != 0;
}

struct qi_qs_state *get_alloc_task_qs(struct task_struct *task)
{
	struct mqos_task *mqos;

	if (!task)
		return NULL;
	mqos = get_alloc_mqos_task(task);
	return mqos ? &mqos->qs : NULL;
}

static struct qi_qs_state *get_task_qs(struct task_struct *task)
{
	struct mqos_task *mqos;

	if (!task)
		return NULL;
	mqos = (void *)(unsigned long)task->android_oem_data1[2];
	return mqos ? &mqos->qs : NULL;
}

/* The stock allocation failure and NULL target dereferences are deliberate. */
static int set_qos_sched(struct task_struct *from, struct task_struct *to,
			bool enable, const struct qs_config *config)
{
	struct qi_qs_state *qs;
	struct sched_param param = { };
	u32 flags, group, policy;
	int prio, ret = 0;

	get_task_struct(to);
	qs = get_alloc_task_qs(to);
	policy = to->policy;
	if (policy != SCHED_NORMAL && policy != SCHED_BATCH && !(qs->flags & 1))
		goto out;
	if (to->flags & PF_EXITING || !enable)
		goto out;
	group = from ? 2 : 1;
	flags = qs->flags;
	if (flags & 1) {
		if (((flags >> 1) & 3) != group)
			qs->flags = flags | 6;
		goto out;
	}
	qs->flags = (flags & 0xffffc03fU) | ((policy & 0xff) << 6);
	policy = config->sched_rr ? SCHED_RR : SCHED_FIFO;
	if (!config->inherit_prio_fork)
		policy |= SCHED_RESET_ON_FORK;
	if (from) {
		prio = from->prio;
		if (prio > 99)
			prio = g_surfaceflinger_prio + 1;
	} else {
		prio = config->prio ? 98 - config->prio : g_surfaceflinger_prio + 1;
	}
	param.sched_priority = 99 - prio;
	ret = sched_setscheduler_nocheck(to, policy, &param);
	trace_qs_set_rt_sched(to, prio, config->sched_rr,
			     config->inherit_prio_fork, ret);
	if (ret < 0)
		goto out;
	/* Preserve the separate stores observed at .text+0x1f80..0x1fbc. */
	flags = (qs->flags & ~7U) | (group << 1) | 1;
	WRITE_ONCE(qs->flags, flags);
	flags = (flags & ~BIT(16)) | ((u32)config->sched_rr << 16);
	WRITE_ONCE(qs->flags, flags);
	flags = (flags & ~BIT(17)) | ((u32)config->inherit_prio_fork << 17);
	WRITE_ONCE(qs->flags, flags);
	flags = (flags & 0xff03ffffU) | ((config->prio & 0x3f) << 18);
	WRITE_ONCE(qs->flags, flags);
out:
	put_task_struct(to);
	return ret;
}

static void restore_rt_sched(struct task_struct *task, struct qi_qs_state *qs)
{
	struct sched_param param = { };
	u32 flags = qs->flags;
	u32 policy = (flags >> 6) & 0xff;

	if (policy == SCHED_NORMAL || policy == SCHED_BATCH) {
		/* Stock ignores a scheduler failure, then clears the saved state. */
		sched_setscheduler_nocheck(task, policy, &param);
		trace_qs_restore_rt_sched(task);
		flags = qs->flags & 0xffffc03fU;
	}
	qs->flags = flags & 0xff00fff8U;
}

static void restore_qos_sched(struct task_struct *task, unsigned long event)
{
	struct qi_qs_state *qs;
	u32 flags, group;

	get_task_struct(task);
	qs = get_task_qs(task);
	if (!qs || !(qs->flags & 9))
		goto out;
	flags = qs->flags;
	group = (flags >> 1) & 3;
	switch (event) {
	case 1:
	case 3:
		pr_err("qs: unexpected event[%lu]\n", event);
		break;
	case 2:
		if (!(flags & 1))
			break;
		if (group == 3)
			qs->flags = (flags & ~6U) | 4;
		else if (group == 1)
			restore_rt_sched(task, qs);
		break;
	case 4:
		if (!(flags & 1))
			break;
		if (group == 3)
			qs->flags = (flags & ~6U) | 2;
		else if (group == 2)
			restore_rt_sched(task, qs);
		break;
	}
out:
	put_task_struct(task);
}

static int qievent_callback(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	struct qi_event *event = data;
	struct qs_config config = { };
	struct qi_qs_state *qs;
	struct task_struct *to;
	struct sched_param param = { };
	u32 attrs, flags, next;
	int ret;

	if (!event)
		return -EINVAL;
	switch (action) {
	case 1:
		if (!g_qs_enable)
			return 0;
		attrs = event->schedattrs;
		config.sched_rr = (attrs >> 8) & 1;
		config.inherit_prio_fork = (attrs >> 9) & 1;
		config.prio = (attrs >> 10) & 0x3f;
		return set_qos_sched(NULL, event->to, (attrs >> 4) & 1, &config);
	case 2:
		restore_qos_sched(event->to, 2);
		return 0;
	case 3:
		qi_intf_schedinfo_update(event);
		if (!g_qs_enable)
			return 0;
		attrs = event->schedattrs;
		config.sched_rr = (attrs >> 8) & 1;
		config.prio = (attrs >> 10) & 0x3f;
		return set_qos_sched(event->from, event->to,
				     (attrs >> 4) & 1, &config);
	case 4:
		qi_intf_schedinfo_reset(event);
		restore_qos_sched(event->to, 4);
		return 0;
	case 5:
		to = event->to;
		get_task_struct(to);
		qs = get_alloc_task_qs(to);
		ret = 0;
		if ((qs->flags & 1) &&
		    (to->policy == SCHED_NORMAL || to->policy == SCHED_BATCH)) {
			param.sched_priority = 98 - g_surfaceflinger_prio;
			ret = sched_setscheduler_nocheck(to,
					SCHED_FIFO | SCHED_RESET_ON_FORK, &param);
			trace_qs_correct_rt_sched(to, g_surfaceflinger_prio + 1, ret);
		}
		put_task_struct(to);
		return ret;
	case 6:
		to = event->to;
		get_task_struct(to);
		ret = -1;
		if ((event->flags & BIT(9)) && to && (qs = get_alloc_task_qs(to))) {
			flags = qs->flags;
			next = ((event->flags >> 4) & 1) | 4;
			WRITE_ONCE(qs->flags, (flags & ~7U) | next);
			next |= (flags & 0x1fff8U) | (((event->flags >> 9) & 1) << 17);
			WRITE_ONCE(qs->flags, (flags & 0xfffc0000U) | next);
			WRITE_ONCE(qs->flags, (flags & 0xff000000U) | next |
				   ((event->flags & 0xfc00U) << 8));
			trace_qs_rt_fork_inherit(event->from, to);
			ret = 0;
		}
		put_task_struct(to);
		return ret;
	default:
		return -EINVAL;
	}
}

static struct notifier_block g_qs_nb = { .notifier_call = qievent_callback };

static void qs_binder_preset(void *unused, struct hlist_head *hhead,
			     struct mutex *lock, struct binder_proc *proc)
{
	struct qi_qs_state *qs;

	if (!proc || !proc->tsk || !(is_qi_feat_enabled(1) & 1))
		return;
	qs = get_alloc_task_qs(proc->tsk);
	if ((qs->flags & 7) == 3) {
		proc->default_priority.sched_policy = SCHED_NORMAL;
		proc->default_priority.prio = 120;
		trace_qs_binder_preset(proc->tsk);
	}
}

static void sl_record_rwsem_starttime(void *unused, struct rw_semaphore *sem,
				     unsigned long settime)
{
	struct qi_qs_state *qs;
	struct mi_task_struct *mi;

	if (settime || !g_sl_enable || !current)
		return;
	qs = get_task_qs(current);
	if (!qs || !(qs->flags & BIT(14)))
		return;
	mi = get_mi_task_struct(current);
	if (mi) {
		rcu_read_lock();
		mi->flags_0xc0 &= 0xf3f51fffU;
		rcu_read_unlock();
	}
	trace_sl_reset_mi_vip(current, (unsigned long)sem);
	qs->deadline = 0;
	qs->flags &= ~BIT(14);
}

static void sl_rwsem_read_wait_start(void *unused, struct rw_semaphore *sem)
{
	struct task_struct *task = current;
	struct qi_qs_state *qs;
	struct mi_task_struct *mi;

	if (!g_sl_enable || is_qos_task(task) || task->prio <= 99)
		return;
	if (get_proc_static_qos_lvl(task) != 3 || !task)
		return;
	qs = get_task_qs(task);
	if (!qs)
		return;
	/* These are literal stock jiffy intervals, not milliseconds. */
	qs->deadline = jiffies + 500;
	mi = get_mi_task_struct(task);
	if (mi) {
		if ((long)(mi->deadline_0x290 - jiffies - 5) < 0)
			mi->deadline_0x290 = jiffies + 5;
		mi->flags_0xc0 |= 0x0c0ae000U;
	}
	/* sl_prio changes the diagnostic value, not the fixed flag mask. */
	trace_sl_set_mi_vip(task, g_sl_prio, (unsigned long)sem);
	qs->flags |= BIT(14);
}

static void qs_scheduler_tick(void *unused, struct rq *rq)
{
	struct task_struct *task = rq->curr;
	struct qi_qs_state *qs;

	/* Stock uses QS enable here even though speedlock has its own switch. */
	if (!is_qs_enabled() || !task)
		return;
	qs = get_task_qs(task);
	if (!qs || !qs->deadline || !(qs->flags & BIT(14)) ||
	    (long)(qs->deadline - jiffies) >= 0)
		return;
	pr_err("sl: [%s pid:%d] speedlock timeout:[%u]ms\n", task->comm,
	       task->pid, jiffies_to_msecs(jiffies - qs->deadline));
	trace_sl_timeout(task);
}

static ssize_t sched_enable_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "QS_ENABLE:\t%d\n", g_qs_enable != 0);
}

static ssize_t sched_enable_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int value = 0;

	if (kstrtouint(buf, 0, &value)) {
		pr_err("qs: sched enable store: parse enable failed\n");
		return -EINVAL;
	}
	g_qs_enable = !!value;
	return count;
}

static ssize_t debug_prio_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "surfacelinger prio:\t%d\n",
			g_surfaceflinger_prio);
}

static ssize_t debug_prio_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int value = 0;

	if (kstrtouint(buf, 0, &value)) {
		pr_err("qs: debug prio store: parse value failed\n");
		return -EINVAL;
	}
	if (value < 1 || value > 99) {
		pr_err("qs: debug prio store: invalid value[%u]\n", value);
		return -EINVAL;
	}
	g_surfaceflinger_prio = value;
	return count;
}

static ssize_t sl_enable_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "enable:\t%d\n", g_sl_enable);
}

static ssize_t sl_enable_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int value = 0;

	if (kstrtouint(buf, 0, &value)) {
		pr_err("sl: speedlock store: parse enable failed\n");
		return -EINVAL;
	}
	g_sl_enable = !!value;
	return count;
}

static ssize_t sl_prio_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "sl prio:\t%d\n", g_sl_prio);
}

static ssize_t sl_prio_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int value = 0;

	if (kstrtouint(buf, 0, &value)) {
		pr_err("sl: sl prio store: parse value failed\n");
		return -EINVAL;
	}
	if (value < 1 || value > 99) {
		pr_err("sl: sl prio store: invalid value[%u]\n", value);
		return -EINVAL;
	}
	g_sl_prio = value;
	return count;
}

static struct kobj_attribute sched_enable_attr =
	__ATTR(sched_enable, 0640, sched_enable_show, sched_enable_store);
static struct kobj_attribute debug_prio_attr =
	__ATTR(debug_prio, 0640, debug_prio_show, debug_prio_store);
static struct kobj_attribute sl_enable_attr =
	__ATTR(sl_enable, 0640, sl_enable_show, sl_enable_store);
static struct kobj_attribute sl_prio_attr =
	__ATTR(sl_prio, 0640, sl_prio_show, sl_prio_store);
static struct attribute *qs_attrs[] = {
	&sched_enable_attr.attr, &debug_prio_attr.attr, NULL,
};
static struct attribute *sl_attrs[] = {
	&sl_enable_attr.attr, &sl_prio_attr.attr, NULL,
};
static const struct attribute_group qs_attr_group = { .attrs = qs_attrs };
static const struct attribute_group sl_attr_group = { .attrs = sl_attrs };

static int sl_init(void)
{
	struct kobject *kobj;
	int ret;

	ret = register_trace_android_vh_record_rwsem_lock_starttime(
			sl_record_rwsem_starttime, NULL);
	if (ret < 0)
		return ret;
	ret = register_trace_android_vh_rwsem_read_wait_start(
			sl_rwsem_read_wait_start, NULL);
	if (ret >= 0) {
		ret = register_trace_android_vh_scheduler_tick(qs_scheduler_tick, NULL);
		if (ret >= 0) {
			kobj = kobject_create_and_add("speedlock", kernel_kobj);
			if (!kobj) {
				ret = -ENOMEM;
			} else {
				ret = sysfs_create_group(kobj, &sl_attr_group);
				if (!ret)
					return 0;
				kobject_put(kobj);
				pr_err("sl: speedlock init: failed to create sys node\n");
			}
		}
	}
	/* Stock unregisters all three even when a later probe was never added. */
	unregister_trace_android_vh_scheduler_tick(qs_scheduler_tick, NULL);
	unregister_trace_android_vh_rwsem_read_wait_start(sl_rwsem_read_wait_start, NULL);
	unregister_trace_android_vh_record_rwsem_lock_starttime(sl_record_rwsem_starttime, NULL);
	return ret;
}

static int __init qs_init(void)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("qos_sched", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &qs_attr_group);
	if (ret) {
		kobject_put(kobj);
		pr_err("qs: qs init: failed to create sys node\n");
		return -EINVAL;
	}
	ret = register_trace_android_vh_binder_preset(qs_binder_preset, NULL);
	if (ret < 0) {
		pr_err("qs: register binder_preset hook failed, err:%d\n", ret);
		goto remove_sysfs;
	}
	ret = register_qievent_notifier(&g_qs_nb);
	if (ret < 0) {
		unregister_trace_android_vh_binder_preset(qs_binder_preset, NULL);
		pr_err("qs: register qievent failed, err:%d\n", ret);
		goto remove_sysfs;
	}
	ret = sl_init();
	if (ret >= 0)
		return ret;
	unregister_qievent_notifier(&g_qs_nb);
	unregister_trace_android_vh_binder_preset(qs_binder_preset, NULL);
	pr_err("qs: speed lock probe failed\n");
remove_sysfs:
	sysfs_remove_group(kobj, &qs_attr_group);
	kobject_put(kobj);
	return ret;
}

module_init(qs_init);
/* Stock has no cleanup_module; do not invent an unload path. */
MODULE_AUTHOR("Peide Du <dupeide@xiaomi.com>");
MODULE_DESCRIPTION("Xiaomi QOS sched Driver");
MODULE_LICENSE("GPL v2");
MODULE_SOFTDEP("pre: xr-qi");
