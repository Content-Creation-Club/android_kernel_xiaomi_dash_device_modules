// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include "metis-final-control.h"

/* __set_common_persist_vip 00127ebc, .text+0x1f494..0x1f6cc. */
int __set_common_persist_vip(s32 pid, struct task_struct *task)
{
	struct mi_task_struct *state;
	int i, free_slot = -1, result;

	for (i = 0; i < 8; i++)
		if (persist_vip_pids[i] == pid)
			return -EEXIST;
	for (i = 0; i < 8; i++)
		if (!persist_vip_pids[i]) {
			free_slot = i;
			break;
		}
	if (free_slot < 0)
		return -ENOSPC;
	persist_vip_pids[free_slot] = pid;
	get_task_struct(task);
	state = get_mi_task_struct(task);
	if (state) {
		state->flags_0xc0 |= 0x20000000;
		result = 0;
	} else {
		persist_vip_pids[free_slot] = 0;
		result = -EFAULT;
	}
	put_task_struct(task);
	return result;
}

/* metis_set_common_persist_vip 00127e20, .text+0x1f3f8..0x1f494. */
void metis_set_common_persist_vip(struct task_struct *task)
{
	int result, pid;

	if (!task) {
		printk("\0016metis-fas: set common persist vip task: but p is NULL\n");
		return;
	}
	pid = task->pid;
	spin_lock(&persist_vip_lock);
	result = __set_common_persist_vip(pid, task);
	spin_unlock(&persist_vip_lock);
	if (!result)
		printk("\0016metis-fas: set common persist vip task: %d %s\n", pid, task->comm);
	else
		printk("\0016metis-fas: set common persist vip task failed: %d, error: %d\n", pid, result);
}

/* set_common_persist_vip_task 0012810c, .text+0x1f6e4..0x1f7bc;
 * param_ops .rodata+0xf58, CFI63fa7f9c establishes unused kp. */
int set_common_persist_vip_task(const char *value,
			      const struct kernel_param *param)
{
	struct task_struct *task;
	u32 pid = 0;
	int result;

	if (sscanf(value, "%u\n", &pid) != 1 || !pid)
		return -EINVAL;
	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (!task) {
		result = -ESRCH;
	} else {
		spin_lock(&persist_vip_lock);
		result = __set_common_persist_vip(pid, task);
		spin_unlock(&persist_vip_lock);
	}
	rcu_read_unlock();
	printk("\0016metis-fas: set common persist vip task: %d, ret: %d\n", pid, result);
	return result;
}

/* get_common_persist_vip_task 001281e8, .text+0x1f7c0..0x1f974;
 * .rodata+0xf60 and CFIc64c3dfc supply the unused param formal. */
int get_common_persist_vip_task(char *buffer, const struct kernel_param *param)
{
	int i, len = 0;

	spin_lock(&persist_vip_lock);
	for (i = 0; i < 8; i++)
		if (persist_vip_pids[i])
			len += sprintf(buffer + len, "%d ", persist_vip_pids[i]);
	spin_unlock(&persist_vip_lock);
	if (!len) {
		strcpy(buffer, "empty\n");
		return 6;
	}
	if (len > 0)
		buffer[len - 1] = '\n';
	return len;
}

/* set_clear_persist_vip_task 001283a0, .text+0x1f978..0x1fba0;
 * stock clears the PID slot before lookup, with spinlock and RCU held. */
int set_clear_persist_vip_task(const char *value,
			     const struct kernel_param *param)
{
	struct task_struct *task;
	struct mi_task_struct *state;
	u32 pid = 0;
	int i;

	if (sscanf(value, "%u\n", &pid) != 1 || !pid)
		return -EINVAL;
	spin_lock(&persist_vip_lock);
	for (i = 0; i < 8; i++) {
		if (persist_vip_pids[i] != pid)
			continue;
		persist_vip_pids[i] = 0;
		rcu_read_lock();
		task = find_task_by_vpid(pid);
		if (task) {
			get_task_struct(task);
			state = get_mi_task_struct(task);
			if (state)
				state->flags_0xc0 &= ~0x20000000U;
			else
				printk("\0014metis-fas: Failed to get mi_task for PID %d\n", pid);
			put_task_struct(task);
		}
		rcu_read_unlock();
		break;
	}
	spin_unlock(&persist_vip_lock);
	return 0;
}

/* Literal stock 001280f8/.text+0x1f6d0 and 00128104/+0x1f6dc;
 * CFI36b1c5a6 and a540670c establish int(void), void(void). */
int metis_common_api_init(void) { return 0; }
void metis_common_api_exit(void) { }
