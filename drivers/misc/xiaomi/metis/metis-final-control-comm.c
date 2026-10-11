// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cgroup.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <trace/hooks/cgroup.h>
#include <trace/hooks/sched.h>
#include "metis-final-control.h"

/* Preserve stock subsystem0 lookup and the name access after RCU unlock;
 * the cgroup/kernfs fields are current native fields, not old offsets. */
static bool task_group_named(struct task_struct *task, const char *wanted)
{
	struct cgroup *group;
	const char *name;

	rcu_read_lock();
	group = task_css_set(task)->subsys[0]->cgroup;
	rcu_read_unlock();
	name = group->kn->name;
	return name && !strcmp(wanted, name);
}

/* str2_in_str1 0012063c, .text+0x17c14..0x17c78. */
bool str2_in_str1(const char *haystack, const char *needle)
{
	if (!strcmp(needle, "*")) return true;
	return strnstr(haystack, needle, strlen(haystack)) != NULL;
}

/* is_main_thread_depend 0011f728, .text+0x16d00..0x16d78. */
bool is_main_thread_depend(struct mi_task_struct *state)
{
	u8 *record = get_important_task_info(state);

	return record && (record[0x18] & 4);
}

/* kernel_set_static_vip 0011f9b8, .text+0x16f90..0x17114. */
int kernel_set_static_vip(struct task_struct *task)
{
	struct mi_task_struct *state;
	int result = -EINVAL;

	if (!task) return result;
	get_task_struct(task);
	state = get_mi_task_struct(task);
	if (state) {
		if (comm_monitor_debug)
			printk("\0016metis-comm-monitor: kernel_set_static_vip: pid=%d \n", task->pid);
		MI_FIELD(state, u32, 0x2f8) = 0;
		mi_dynamic_vtask_prio_sched_start(task, 2000);
		result = 0;
	}
	put_task_struct(task);
	return result;
}

/* func_set_static_vip 0011fca4, .text+0x1727c..0x17474;
 * param_ops .rodata+0xbe0 and CFI63fa7f9c provide unused kp. */
int func_set_static_vip(const char *value, const struct kernel_param *param)
{
	struct task_struct *task;
	struct mi_task_struct *state;
	u32 pid = 0;
	int result = -EINVAL;

	if (sscanf(value, "%u\n", &pid) != 1) return result;
	set_static_vip = pid;
	rcu_read_lock();
	task = find_task_by_vpid(set_static_vip);
	if (!task || !refcount_read(&task->usage)) {
		rcu_read_unlock();
		return result;
	}
	get_task_struct(task);
	rcu_read_unlock();
	state = get_mi_task_struct(task);
	if (state) {
		if (comm_monitor_debug)
			printk("\0016metis-comm-monitor: func_set_static_vip: pid=%d \n", task->pid);
		MI_FIELD(state, u32, 0x2f8) = 0;
		mi_dynamic_vtask_prio_sched_start(task, 2000);
		result = 0;
	}
	put_task_struct(task);
	return result;
}

/* probe_task_rename 0011fea0, .text+0x17478..0x177ac.
 * Native restricted hook declares (void *, task *, bool exec). Original
 * CFI1a322d69 is preserved as evidence; unused exec was lost by Ghidra. */
void probe_task_rename(void *unused, struct task_struct *task, bool exec)
{
	struct mi_task_struct *state, *leader;
	struct metis_comm_work *item;
	int enabled = get_mi_fboost_enable();

	if (!task || !enabled || !refcount_read(&task->usage)) return;
	get_task_struct(task);
	state = get_mi_task_struct(task);
	if (mi_task_uid(state) != 1000 && !task_group_named(task, "foreground")) {
		state = get_mi_task_struct(task);
		if ((state && (state->flags_0xc0 & 0x800)) ||
		    !task_group_named(task, "top-app"))
			goto put;
	}
	if (comm_monitor_debug)
		printk("\0016metis-comm-monitor: probe_task_rename: pid=%d task_name=%s parent_name=%s \n",
			task->pid, task->comm, task->group_leader->comm);
	state = get_mi_task_struct(task);
	if (!state) goto put;
	leader = get_mi_task_struct(task->group_leader);
	if (!leader) goto put;
	item = kmalloc(sizeof(*item), GFP_ATOMIC);
	if (!item) goto put;
	item->task = task;
	item->state = state;
	item->leader_state = leader;
	INIT_WORK(&item->work, compare_task_comm_work_func);
	get_task_struct(task->group_leader);
	/* queue result ignored; no queue failure reference release in stock. */
	queue_work(my_wq, &item->work);
	return;
put:
	put_task_struct(task);
}

/* probe_android_vh_cgroup_set_task 001201d8, .text+0x177b0..0x17960;
 * native hook formals (void *, int, task *), stock CFIef405919. */
void probe_android_vh_cgroup_set_task(void *unused, int result,
				    struct task_struct *task)
{
	struct mi_task_struct *state;
	struct metis_comm_work *item;

	if (!get_mi_fboost_enable()) return;
	if (task && comm_monitor_debug)
		printk("\0016metis-comm-monitor: probe_android_vh_cgroup_set_task: pid=%d task_name=%s parent_name=%s is_top_app=%d \n",
			task->pid, task->comm, task->group_leader->comm,
			task_group_named(task, "top-app"));
	if (result || !task || !(state = get_mi_task_struct(task)) ||
	    !MI_FIELD(state, u8, 0x2e0)) return;
	item = kmalloc(sizeof(*item), GFP_ATOMIC);
	if (!item) return;
	get_task_struct(task);
	item->task = task;
	item->top_app = task_group_named(task, "top-app");
	INIT_WORK(&item->work, setup_task_wk_func);
	queue_work(my_wq, &item->work);
}

/* compare_task_comm_work_func 0012038c, .text+0x17964..0x17c14.
 * Real work callback CFIb86001b6; no copied work_struct prefix. */
void compare_task_comm_work_func(struct work_struct *work)
{
	struct metis_comm_work *item = container_of(work, struct metis_comm_work, work);
	struct task_struct *task = item->task;
	struct mi_task_struct *state = item->state, *leader = item->leader_state;
	u8 *record;
	u32 flags;
	int i;

	for (i = 0; i < 7; i++) {
		record = important_tasks[i].raw;
		if (record[0x10] || !str2_in_str1(task->comm, MI_FIELD(record, const char *, 0)) ||
		    !str2_in_str1(MI_FIELD(record, const char *, 8), task->group_leader->comm))
			continue;
		if (!strcmp(MI_FIELD(record, const char *, 0), "*"))
			MI_FIELD(leader, u32, 0x2fc) = i;
		flags = MI_FIELD(record, u32, 0x18);
		if (flags & 1) {
			MI_FIELD(state, u32, 0x2f8) = i;
			MI_FIELD(leader, u8, 0x2e0) = 1;
		} else {
			if ((flags & 2) && MI_FIELD(record, u32, 0x14) == (u32)mi_task_uid(state)) {
				MI_FIELD(state, u32, 0x2f8) = i;
			} else if (flags & (4 | 8 | 16)) {
				MI_FIELD(state, u32, 0x2f8) = i;
			} else {
				continue;
			}
		}
		if (state && (is_static_vip(state) || is_top_app_vip(state))) {
			mi_dynamic_vtask_prio_sched_start(task, 2000);
			if (comm_monitor_debug)
				printk("\0016metis-comm-monitor: set vip: pid=%d task_name=%s \n", task->pid, task->comm);
		}
		break;
	}
	put_task_struct(item->task);
	/* The retained leader state is dereferenced after releasing task. */
	put_task_struct(item->leader_state->task);
	kfree(item);
}

/* setup_task_wk_func 001206a4, .text+0x17c7c..0x17e5c.
 * Stock walks signal.thread_head unlocked and does not take thread refs. */
void setup_task_wk_func(struct work_struct *work)
{
	struct metis_comm_work *item = container_of(work, struct metis_comm_work, work);
	struct task_struct *task;
	struct mi_task_struct *state;
	bool vip;

	list_for_each_entry(task, &item->task->signal->thread_head, thread_node) {
		state = get_mi_task_struct(task);
		if (!state || !is_top_app_vip(state)) continue;
		state = get_mi_task_struct(task);
		vip = state && (state->flags_0xc0 & 0x800);
		if (item->top_app && !vip) {
			mi_dynamic_vtask_prio_sched_start(task, 2000);
			if (comm_monitor_debug)
				printk("\0016metis-comm-monitor: cgroup_chaned set vip: pid=%d task_name=%s parent_name=%s \n",
					task->pid, task->comm, task->group_leader->comm);
		} else if (!item->top_app && vip) {
			mi_dynamic_vtask_prio_sched_end(task);
			if (comm_monitor_debug)
				printk("\0016metis-comm-monitor: cgroup_chaned clear vip: pid=%d task_name=%s parent_name=%s \n",
					task->pid, task->comm, task->group_leader->comm);
		}
	}
	put_task_struct(item->task);
	kfree(item);
}

/* metis_comm_monitor_init 0011fc24/.text+0x171fc; original WQ flags2
 * are WQ_UNBOUND, max_active1. Registration errors are ignored. */
int metis_comm_monitor_init(void)
{
	my_wq = alloc_workqueue("metis_comm_monitor", WQ_UNBOUND, 1);
	if (!my_wq) return -1;
	register_trace_android_rvh_set_task_comm(probe_task_rename, NULL);
	register_trace_android_vh_cgroup_set_task(probe_android_vh_cgroup_set_task, NULL);
	return 0;
}

/* metis_comm_monitor_exit 0011fc9c/.text+0x17274 is genuinely RET-only;
 * no hook unregister, flushing or workqueue destruction exists. */
void metis_comm_monitor_exit(void) { }
