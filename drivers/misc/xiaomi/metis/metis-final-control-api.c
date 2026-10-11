// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include "metis-final-control.h"

/* mi_vip_task_req 0011b904, .text+0x12edc..0x1311c. */
void mi_vip_task_req(u32 *pids, u32 count, u32 duration)
{
	struct task_struct *task;
	struct mi_task_struct *state;
	unsigned long deadline;
	u32 i;

	if (count >= 6) {
		printk("\0013metis-fas: req too many vip tasks\n");
		return;
	}
	if (!pids) return;
	if (duration > 14999) duration = 15000;
	if (!duration) duration = 30;
	for (i = 0; i < count; i++) {
		rcu_read_lock();
		task = find_task_by_vpid(pids[i]);
		if (!task) {
			rcu_read_unlock();
			printk("\0013metis-fas: Invalid input %d, no such process\n", pids[i]);
			continue;
		}
		get_task_struct(task);
		printk("\0016metis-fas: %d, %s set as mi vip task from %lu to %lu",
			pids[i], task->comm, jiffies, jiffies + duration);
		rcu_read_unlock();
		state = get_mi_task_struct(task);
		if (state) {
			deadline = jiffies + duration;
			if (deadline > state->deadline_0x290)
				state->deadline_0x290 = deadline;
			if (!(state->flags_0xc0 & 0x100000))
				atomic_inc(&mi_dynamic_vip_num);
			state->flags_0xc0 |= 0x2100000;
		}
		put_task_struct(task);
	}
}

/* set_mi_vip_task_req 0011b574, .text+0x12b4c..0x12e18.
 * .rodata+0x7f8/CFI63fa7f9c provides native two-formal parameter callback. */
int set_mi_vip_task_req(const char *value, const struct kernel_param *param)
{
	const char *p = value, *end, *delimiter;
	u32 pid = 0, duration = 0, index;
	int colons = -1;

	do { p = strchr(p + 1, ':'); colons++; } while (p);
	if (!colons) {
		if (sscanf(value, "%u-%u\n", &pid, &duration) != 2) return -EINVAL;
		printk("\0016metis-fas: val %d times %d\n", pid, duration);
		mi_viptask[0] = pid;
		mi_vip_task_req(&pid, 1, duration);
		return 0;
	}
	p = value;
	end = value + (int)strlen(value);
	for (index = 0; index < 3 && index < (u32)colons; index++) {
		if (sscanf(p, "%u", &pid) != 1) return -EINVAL;
		mi_viptask[index] = pid;
		printk("\0016metis-fas: arg %d val %d\n", index + 1, pid);
		delimiter = strchr(p + 1, ':');
		p = delimiter + 1;
		if (p >= end) return 0;
	}
	if (colons <= 2) {
		if (p < end) {
			if (sscanf(p, "%u-%u", &pid, &duration) != 2) return -EINVAL;
			mi_viptask[index] = pid;
			printk("\0016metis-fas: arg %d val = %d times = %d\n", index + 1, pid, duration);
			index++;
		}
		mi_vip_task_req(mi_viptask, index, duration);
		return 0;
	}
	/* Fourth and later segments supply a duration only; pids stop at three. */
	if (p < end) {
		p = strchr(p + 1, '-');
		if (p + 1 < end) {
			if (sscanf(p + 1, "%u", &duration) != 1) return -EINVAL;
			printk("\0016metis-fas: arg %d times %d\n", 3, duration);
			mi_vip_task_req(mi_viptask, 3, duration);
		}
	}
	return 0;
}

/* get_mi_viptask 0011b844, .text+0x12e1c..0x12edc; callback CFIc64c3dfc. */
int get_mi_viptask(char *buffer, const struct kernel_param *param)
{
	int i, len = 0;

	for (i = 0; i < 3; i++)
		len += snprintf(buffer + len, 0x1000 - (long)len, "%u:%d ",
				mi_viptask[i], atomic_read(&mi_dynamic_vip_num));
	return len + snprintf(buffer + len, 0x1000 - (long)len, "\n");
}

/* set_mi_efboost 0011be54, .text+0x1342c..0x13674. Clearing decrements
 * counters based on the requested mask, even if the task never held them. */
int set_mi_efboost(u32 pid, unsigned long mask, bool clear)
{
	struct task_struct *task;
	struct mi_task_struct *state;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (!task) {
		rcu_read_unlock();
		printk("\0013metis-fas: Invalid input %d, no such process\n", pid);
		return 1;
	}
	get_task_struct(task);
	rcu_read_unlock();
	if (!clear) {
		set_mi_vip_task(task, UINT_MAX, mask);
	} else {
		state = get_mi_task_struct(task);
		if (state) {
			if (mask & 0x100000) atomic_dec(&mi_dynamic_vip_num);
			if (mask & 0x400000) atomic_dec(&mi_sdk_eboost_vip_num);
			if (mask & 0x200000) MI_FIELD(state, u32, 0x298) = UINT_MAX;
			state->flags_0xc0 &= ~(u32)mask | 0x2000000;
			if (metis_debug && mask == 0x400000)
				printk("\0016metis-fas: clean mi eboost vip task: cnt = %d, pid = %d, name = %s",
					atomic_read(&mi_sdk_eboost_vip_num), task->pid, task->comm);
		}
	}
	put_task_struct(task);
	if (metis_debug) printk("\0013metis-fas: pid: %d, mask: %lu.", pid, mask);
	return 0;
}

/* set_efboost 0011bc0c, .text+0x131e4..0x1342c. A failed task lookup
 * consumes the colon budget without advancing the input cursor. */
int set_efboost(const char *value, const struct kernel_param *param)
{
	const char *p = value, *end;
	unsigned long mask = 0;
	bool clear = false;
	u32 pid = 0;
	int colons = -1, i;

	if (strstr(param->name, "add_mi_viptask_sched_priority")) mask = 0x400000;
	else if (strstr(param->name, "add_mi_viptask_enqueue_boost")) mask = 0x200000;
	else if (strstr(param->name, "del_mi_viptask_enqueue_boost")) { mask = 0x200000; clear = true; }
	else if (strstr(param->name, "del_mi_viptask_sched_priority")) { mask = 0x400000; clear = true; }
	else if (strstr(param->name, "del_mi_viptask_sched_lit_core")) { mask = 0x10; clear = true; }
	else if (strstr(param->name, "add_mi_viptask_sched_lit_core")) mask = 0x10;
	do { p = strchr(p + 1, ':'); colons++; } while (p);
	if (!colons) {
		if (sscanf(value, "%u\n", &pid) != 1) return -EINVAL;
		set_mi_efboost(pid, mask, clear);
		return 0;
	}
	p = value;
	end = value + (int)strlen(value);
	for (i = 0; i < colons; i++) {
		if (sscanf(p, "%u", &pid) != 1) return -EINVAL;
		if (!set_mi_efboost(pid, mask, clear)) {
			p = strchr(p + 1, ':') + 1;
			if (p >= end) break;
		}
	}
	if (p < end) {
		if (sscanf(p, "%u", &pid) != 1) return -EINVAL;
		set_mi_efboost(pid, mask, clear);
	}
	return 0;
}

/* set_therm_break_enable 0011c0a0/.text+0x13678; get_therm_break_enable
 * 0011c14c/+0x13724. Getter returns knob as a byte count without a string. */
int set_therm_break_enable(const char *value, const struct kernel_param *param)
{
	u32 enabled = 0;

	if (sscanf(value, "%u\n", &enabled) != 1) return -EINVAL;
	if (strstr(param->name, "is_break_enable")) thermal_break_enable = enabled;
	return 0;
}
int get_therm_break_enable(char *buffer, const struct kernel_param *param)
{
	return strstr(param->name, "is_break_enable") ? thermal_break_enable : 0;
}

/* proc_open_fas 0011c1a0/.text+0x13778; proc_show_fas 0011c2dc/+0x138b4.
 * proc ops .rodata+0x858 supplies open; seq callback CFI d7b572f7. */
int proc_open_fas(struct inode *inode, struct file *file)
{
	return single_open(file, proc_show_fas, NULL);
}
int proc_show_fas(struct seq_file *seq, void *unused)
{
	seq_printf(seq, "forground app uid:%d\n", foreground_uid);
	return 0;
}

/* proc_write_fas 0011c1d0, .text+0x137a8..0x138b4. Original zeroes 13
 * bytes, permits at most12 copied bytes, and returns the clamped length. */
ssize_t proc_write_fas(struct file *file, const char __user *buffer,
		       size_t count, loff_t *ppos)
{
	char input[13] = { 0 };
	int uid = 0, result;

	if (count > 11) count = 12;
	if (copy_from_user(input, buffer, count)) return -EFAULT;
	result = kstrtoint(strim(input), 0, &uid);
	if (result < 0) return result;
	if (!result) {
		if (uid < 10000) return -EINVAL;
		if (metis_debug) printk("\0016metis-fas: Change foreground uid from %d to %d\n", foreground_uid, uid);
		foreground_uid = uid;
	}
	return count;
}

static bool active_frame_window(void)
{
	unsigned long duration = 0;
	s64 start = frame_boost_start;

	if (vtask_monitor_beg_us >= start || vtask_monitor_end_us >= vtask_monitor_beg_us)
		return false;
	get_frame_boost_end(&duration);
	return (s64)((u64)ktime_to_us(ktime_get()) - (u64)start - duration * 1000) < 0;
}

/* metis_set_vip_thread 0011d57c, .text+0x14b54..0x14d64. */
void metis_set_vip_thread(struct task_struct *task, u32 kind, u32 seconds)
{
	struct mi_task_struct *state;
	u32 delta;

	if (!task || !(state = get_mi_task_struct(task))) return;
	MI_FIELD(state, u64, 0x278) = jiffies + 250UL;
	switch (kind) {
	case 0:
	case 1:
		if (active_frame_window()) {
			if ((u32)(task->policy - 1) > 1) state->flags_0xc0 |= 0x1000;
		} else if (state->flags_0xc0 & 0x1000) state->flags_0xc0 &= ~0x1000U;
		mi_set_fas_req(state);
		state->flags_0xc0 |= kind ? 0x400 : 0x100;
		break;
	case 7:
		delta = seconds < 121 ? max(seconds, 1U) * 250U - 250U : 29750U;
		MI_FIELD(state, u64, 0x278) += delta;
		fallthrough;
	default:
		mi_set_fas_req(state);
		state->flags_0xc0 |= 0x800;
		break;
	case 15:
		state->flags_0xc0 |= 0x800000;
		mi_set_fas_req(state);
		break;
	case 16:
		state->flags_0xc0 |= 0x1000000;
		mi_set_fas_req(state);
		break;
	}
}

static void ioctl_foreground(u32 uid)
{
	if (metis_debug) printk("\0016metis-fas: Change foreground uid from %d to %d\n", foreground_uid, uid);
	if (uid != last_traced_uid) {
		show_traced_uid = uid;
		last_traced_uid = uid;
	}
	foreground_uid = uid;
}

/* metis_ioctl 0011cbac, .text+0x14184..0x14b30; true fops long return
 * (CFIe01408cd), .rodata+0x9f8/+0xa00. Dispatch uses only cmd low8. */
long metis_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct task_struct *task = current;
	struct mi_task_struct *state;
	u32 request[2] = { 0 }, uid;
	int pid;
	bool touch;

	if (!mi_fboost_enable) return -1;
	uid = __kuid_val(current_uid());
	if (metis_debug)
		printk("\0013metis-fas: metis comming. uid %d, pid %d, %d %d %llu",
			uid, current->pid, 0xa91, cmd & 0xff, ktime_to_us(ktime_get()));
	switch (cmd & 0xff) {
	case 0:
		if (active_frame_window()) {
			if (metis_debug) printk("\0016metis-fas: UI_GET_VTASK_TOKEN, in_frame_boost");
			del_cluster();
		}
		vtask_monitor = 1;
		vtask_monitor_beg_us = ktime_to_us(ktime_get());
		ui_last_cpu = raw_smp_processor_id();
		ioctl_foreground(uid);
		metis_set_vip_thread(task, 0, 1);
		break;
	case 1:
		ren_last_cpu = raw_smp_processor_id();
		ioctl_foreground(uid);
		metis_set_vip_thread(task, 1, 1);
		break;
	case 2:
		ioctl_foreground(uid);
		if (task && (state = get_mi_task_struct(task))) {
			MI_FIELD(state, u64, 0x278) = jiffies + 250UL;
			mi_set_fas_req(state);
			state->flags_0xc0 |= 0x800;
		}
		break;
	case 3:
		if (active_frame_window()) {
			if (metis_debug) printk("\0016metis-fas: UI_RELEASE_VTASK_TOKEN, in_frame_boost");
			del_cluster();
		}
		vtask_monitor = 0;
		vtask_monitor_end_us = ktime_to_us(ktime_get());
		if (metis_debug)
			printk("\0016metis-fas: vtask_monitor_us is %lu\n", (unsigned long)(vtask_monitor_end_us - vtask_monitor_beg_us));
		break;
	case 6:
		frame_boost_start = ktime_to_us(ktime_get());
		if (active_frame_window()) {
			if (metis_debug)
				printk("\0016metis-fas: frame_boost_start: %llu, begin:%llu, end:%llu",
					frame_boost_start, vtask_monitor_beg_us, vtask_monitor_end_us);
			thermal_limit_freq[1] = max_limit_freq[1];
			restore_boost_cluster(1);
			thermal_limit_freq[2] = max_limit_freq[2];
			restore_boost_cluster(2);
			boost_cluster(cpu_to_cluster(ui_last_cpu), 1, 1, 0);
			if (metis_debug)
				printk("thermal boost limit 1 %lu, 2 %lu\n",
					thermal_limit_freq[1], thermal_limit_freq[2]);
		}
		break;
	case 7:
		if ((cmd & 0x3fff0000) != 0x80000) return -EINVAL;
		if (copy_from_user(request, (void __user *)arg, 8)) return -EFAULT;
		rcu_read_lock();
		task = find_task_by_vpid(request[0]);
		if (!task) {
			rcu_read_unlock();
			printk("\0013metis-fas: Invalid input %d, no such process\n", request[0]);
			return -EINVAL;
		}
		get_task_struct(task);
		if (metis_debug)
			printk("\0016metis-fas: %d, %s set as mi vip task from %lu to %lu",
				request[0], task->comm, jiffies, jiffies + (u32)(request[1] * 250U));
		rcu_read_unlock();
		metis_set_vip_thread(task, 7, request[1]);
		put_task_struct(task);
		break;
	case 8:
	case 9:
	case 10:
		if (copy_from_user(request, (void __user *)arg, 8)) return -EFAULT;
		pid = (s32)request[0];
		rcu_read_lock();
		if ((cmd & 0xff) == 9) {
			if (pid > 0 && pid != current->pid) task = find_task_by_vpid(pid);
		} else if (pid && pid != current->pid) {
			if (pid < 1) goto invalid_task;
			task = find_task_by_vpid(pid);
		}
		if (!task || !refcount_read(&task->usage)) goto invalid_task;
		get_task_struct(task);
		rcu_read_unlock();
		if ((cmd & 0xff) == 8) {
			if (metis_debug)
				printk("\0016metis-fas: metis ioctl: set dynamic vip, pid = %d, name = %s, time = %d",
					task->pid, task->comm, request[1]);
			mi_dynamic_vtask_prio_sched_start(task, request[1]);
		} else if ((cmd & 0xff) == 9) {
			clean_mi_dynamic_vip_task(task);
			put_task_struct(task);
			/* Preserve original debug-after-put lifetime defect. */
			if (metis_debug) printk("\0016metis-fas: metis ioctl: clear dynamic vip, pid = %d, name = %s\n", task->pid, task->comm);
			break;
		} else {
			if (metis_debug)
				printk("\0016metis-fas: metis ioctl: req async binder link, task = %s(%d), time = %d",
					task->comm, task->pid, request[1]);
			mi_req_async_binder_link_sched_start(task, request[1]);
		}
		put_task_struct(task);
		break;
	case 11:
		if (metis_debug) printk("\0016metis-fas: metis ioctl: cancel async binder link, task = %s(%d)", task->comm, task->pid);
		if (task && (state = get_mi_task_struct(task))) {
			state->flags_0xc0 &= ~0x10000U;
			if (metis_debug) printk("\0016metis-fas: clear async binder link task flag, pid = %d, name = %s", task->pid, task->comm);
		}
		break;
	case 15:
	case 16:
		if ((cmd & 0xff) == 15) sf_last_cpu = raw_smp_processor_id();
		else hwc_last_cpu = raw_smp_processor_id();
		touch = speed_touch_enable && (trace_motion_event == 1 ||
			(trace_motion_event == 2 && ktime_to_us(ktime_get()) < touch_up_start + 150000));
		if (touch && task && (state = get_mi_task_struct(task))) {
			MI_FIELD(state, u64, 0x278) = jiffies + 250UL;
			state->flags_0xc0 |= (cmd & 0xff) == 15 ? 0x800000 : 0x1000000;
			mi_set_fas_req(state);
		}
		break;
	case 17:
	case 18:
		if (speed_touch_enable && (trace_motion_event == 1 ||
			(trace_motion_event == 2 && ktime_to_us(ktime_get()) < touch_up_start + 150000)))
			metis_clean_vip_thread(task);
		break;
	default:
		break;
	}
	return 0;
invalid_task:
	rcu_read_unlock();
	return -EINVAL;
}

/* Literal stock callbacks 0011d55c/+0x14b34, 0011d568/+0x14b40,
 * 0011d574/+0x14b4c. fops relocations provide all unused formals. */
int metis_mmap(struct file *file, struct vm_area_struct *vma) { return 0; }
int metis_open(struct inode *inode, struct file *file) { return 0; }
int metis_release(struct inode *inode, struct file *file) { return 0; }
