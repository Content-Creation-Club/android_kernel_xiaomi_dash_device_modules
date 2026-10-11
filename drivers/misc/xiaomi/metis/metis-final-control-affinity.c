// SPDX-License-Identifier: GPL-2.0-only
#include <linux/compat.h>
#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/user.h>
#include <linux/string.h>
#include <linux/sysctl.h>
#include <linux/uidgid.h>
#include "metis-final-control.h"

/* Helpers extract repeated stock instructions, and count zero stock bodies. */
static bool control_allowed(void)
{
	return !cluaff_control ||
		strnstr(current->comm, "PowerKeeper", strlen(current->comm));
}

/* Original .rodata+0x378..0x398 strings, read in the retained Ghidra image. */
static const char *const cluster_names[4] = {
	"NOP_CLUSTER", "LIT_CLUSTER", "MID_CLUSTER", "BIG_CLUSTER"
};

static void bitmap_clear_cluster(u32 value, int cluster,
				 unsigned long *big, unsigned long *middle,
				 unsigned long *little)
{
	if (cluster == 1)
		clear_bit(value, little);
	else if (cluster == 3)
		clear_bit(value, big);
	else if (cluster == 2)
		clear_bit(value, middle);
	else {
		clear_bit(value, big);
		clear_bit(value, middle);
		clear_bit(value, little);
	}
}

static void bitmap_set_cluster(u32 value, int cluster,
			      unsigned long *big, unsigned long *middle,
			      unsigned long *little)
{
	if (cluster == 1) {
		clear_bit(value, big);
		clear_bit(value, middle);
		set_bit(value, little);
	} else if (cluster == 3) {
		clear_bit(value, little);
		clear_bit(value, middle);
		set_bit(value, big);
	} else if (cluster == 2) {
		clear_bit(value, big);
		clear_bit(value, little);
		set_bit(value, middle);
	} else {
		clear_bit(value, little);
		clear_bit(value, middle);
		clear_bit(value, big);
	}
}

static int colon_count(const char *value)
{
	const char *p = value;
	int count = -1;

	do {
		p = strchr(p + 1, ':');
		count++;
	} while (p);
	return count;
}

/* clear_rebind_task_list 0010e3fc, .text+0x59d4..0x5b90. */
void clear_rebind_task_list(u32 pid, int cluster)
{
	bitmap_clear_cluster(pid, cluster, rebind_task_blist,
			     rebind_task_mlist, rebind_task_llist);
	if (tsched_debug)
		printk("\0016metis-clu-aff: Del pid %d from %d list\n", pid, cluster);
}

/* add_to_rebind_task_list 0010e5b8, .text+0x5b90..0x6068.
 * No internal pid bound; the task is released before bitmap updates. */
void add_to_rebind_task_list(u32 pid, u32 cluster)
{
	struct task_struct *task;
	bool compat;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (!task) {
		rcu_read_unlock();
		return;
	}
	get_task_struct(task);
	if (tsched_debug) {
		if (cluster > 3) {
			asm volatile("brk #1");
			__builtin_unreachable();
		}
		printk("\0016metis-clu-aff: %d, %s  %s to %s",
			pid, task->comm, "add ", cluster_names[cluster]);
	}
	rcu_read_unlock();
	compat = is_compat_thread(task_thread_info(task));
	if (compat && tsched_debug) {
		if (cluster > 3) {
			asm volatile("brk #1");
			__builtin_unreachable();
		}
		printk("\0016metis-clu-aff: INVALID: 32 bit process %d, %s  %s to %s",
			pid, task->comm, "add ", cluster_names[cluster]);
	}
	put_task_struct(task);
	if (compat)
		return;
	bitmap_set_cluster(pid, cluster, rebind_task_blist,
			   rebind_task_mlist, rebind_task_llist);
	if (tsched_debug)
		printk("\0016metis-clu-aff: Add pid %d to %d list\n", pid, cluster);
}

/* rebind_task_req 0010ea90, .text+0x6068..0x62fc. The stock +0xd0
 * private mask and +0xd8 write are retained, with no kernel layout copy. */
void rebind_task_req(u32 *pids, u32 count, u32 cluster, bool unbind)
{
	struct task_struct *task, *owner;
	struct mi_task_struct *state;
	u64 saved;
	u32 i;

	if ((s32)count >= 6) {
		printk("\0013metis-clu-aff: req too many rebind tasks\n");
		return;
	}
	if (!pids || (s32)count <= 0)
		return;
	for (i = 0; i < count; i++) {
		rcu_read_lock();
		task = find_task_by_vpid(pids[i]);
		if (!task) {
			rcu_read_unlock();
			continue;
		}
		get_task_struct(task);
		if (tsched_debug) {
			if (cluster > 3) {
				asm volatile("brk #1");
				__builtin_unreachable();
			}
			printk("\0016metis-clu-aff: %d, %s  %s to %s",
				pids[i], task->comm, unbind ? "unbind" : "rebind", cluster_names[cluster]);
		}
		rcu_read_unlock();
		state = get_mi_task_struct(task);
		if (!state || is_compat_thread(task_thread_info(task)))
			goto put;
		saved = MI_FIELD(state, u64, 0xd8);
		owner = state->task; /* Stock assumes this is non-NULL. */
		if (!is_compat_thread(task_thread_info(owner))) {
			if (cluster > 3) {
				asm volatile("brk #1");
				__builtin_unreachable();
			}
			MI_FIELD(state, u64, 0xd8) = cpumask_bits(&cluster_affinity_cpumask[cluster])[0];
			if (is_compat_thread(task_thread_info(owner))) {
				if (bug_detect) {
					asm volatile("brk #0x800");
					__builtin_unreachable();
				}
				MI_FIELD(state, u64, 0xd8) = saved;
			}
		}
		state->flags_0xc0 &= ~2U;
		set_cpus_allowed_ptr(task, &MI_FIELD(state, cpumask_t, 0xd0));
		/* Stock weights only W0 of the first native cpus_mask word. */
		task->nr_cpus_allowed = hweight64((u32)cpumask_bits(&task->cpus_mask)[0]);
		state->flags_0xc0 = (state->flags_0xc0 & ~4U) | (unbind ? 0 : 4);
put:
		put_task_struct(task);
	}
}

/* set_rebind_task 0010dc0c, .text+0x51e4..0x56cc; native kernel_param
 * set callback from .rodata+0x2b0, CFI63fa7f9c. */
int set_rebind_task(const char *value, const struct kernel_param *param)
{
	const char *p, *end;
	u32 pid = 0, cluster, nr = 0;
	int colons;
	bool reset = false, unbind = false;

	printk("\0013metis-clu-aff: set_rebind_task comm is : %s, pid is : %d\n", current->comm, current->pid);
	if (!control_allowed())
		return 0;
	if (strstr(param->name, "add_rebind_task_lit")) cluster = 1;
	else if (strstr(param->name, "add_rebind_task_mid")) cluster = 2;
	else if (strstr(param->name, "add_rebind_task_big")) cluster = 3;
	else if (strstr(param->name, "reset_rebind_task")) { cluster = 0; reset = true; }
	else if (strstr(param->name, "del_rebind_task_lit")) { cluster = 1; unbind = true; }
	else if (strstr(param->name, "del_rebind_task_mid")) { cluster = 2; unbind = true; }
	else if (strstr(param->name, "del_rebind_task_big")) { cluster = 3; unbind = true; }
	else return 0;
	colons = colon_count(value);
	end = value + (int)strlen(value);
	if (reset) {
		memset(rebind_task_blist, 0, 0x1000);
		memset(rebind_task_mlist, 0, 0x1000);
		memset(rebind_task_llist, 0, 0x1000);
		return 0;
	}
	if (tsched_debug)
		printk("\0016metis-clu-aff: type is %d\n", cluster);
	memset(mi_rebind_task[cluster], 0, 0x14);
	if (!colons) {
		if (sscanf(value, "%u\n", &pid) != 1)
			return -EINVAL;
		printk("\0016metis-clu-aff: pid %d %s to %d\n",
			pid, unbind ? "unbind" : "rebind", cluster);
		mi_rebind_task[cluster][0] = pid;
		if (pid >> 15)
			return -EINVAL;
		if (unbind) clear_rebind_task_list(pid, cluster);
		else add_to_rebind_task_list(pid, cluster);
		rebind_task_req(mi_rebind_task[cluster], 1, cluster, unbind);
		return 0;
	}
	p = value;
	while (nr < (u32)colons) {
		if (sscanf(p, "%u", &pid) != 1 || pid >> 15)
			return -EINVAL;
		if (unbind) clear_rebind_task_list(pid, cluster);
		else add_to_rebind_task_list(pid, cluster);
		mi_rebind_task[cluster][nr] = pid;
		printk("\0016metis-clu-aff: arg %d val %d type %d\n", nr + 1, pid, cluster);
		p = strchr(p + 1, ':') + 1;
		if (p >= end)
			return 0; /* Stock trailing ':' bypasses task-affinity update. */
		if (nr == 4) {
			rebind_task_req(mi_rebind_task[cluster], 5, cluster, unbind);
			return 0;
		}
		nr++;
	}
	if (p < end) {
		if (sscanf(p, "%u", &pid) != 1 || pid >> 15)
			return -EINVAL;
		if (unbind) clear_rebind_task_list(pid, cluster);
		else add_to_rebind_task_list(pid, cluster);
		if ((u64)cluster * 20 + (u64)colons * 4 > 0x4f) {
			asm volatile("brk #1");
			__builtin_unreachable();
		}
		mi_rebind_task[cluster][colons] = pid;
		nr = colons + 1;
		printk("\0016metis-clu-aff: arg %d val = %d type = %d\n", nr, pid, cluster);
	}
	rebind_task_req(mi_rebind_task[cluster], nr, cluster, unbind);
	return 0;
}

/* Helpers deliberately retain snprintf's would-have-written lengths. */
static int list_bitmap(char *buffer, unsigned long *bits, u32 max)
{
	u32 bit;
	int len = 0;

	for_each_set_bit(bit, bits, max)
		len += snprintf(buffer + len, 0x1000 - (long)len, "%u ", bit);
	return len + snprintf(buffer + len, 0x1000 - (long)len, "\n");
}

/* get_rebind_task 0010e0f8, .text+0x56d0..0x59d4. */
int get_rebind_task(char *buffer, const struct kernel_param *param)
{
	printk("\0013metis-clu-aff: get_rebind_task comm is : %s, pid is : %d\n", current->comm, current->pid);
	if (!control_allowed()) return 0;
	if (strstr(param->name, "show_rebind_task_lit"))
		return list_bitmap(buffer, rebind_task_llist, 0x8000);
	if (strstr(param->name, "show_rebind_task_mid"))
		return list_bitmap(buffer, rebind_task_mlist, 0x8000);
	if (strstr(param->name, "show_rebind_task_big"))
		return list_bitmap(buffer, rebind_task_blist, 0x8000);
	strcpy(buffer, "need show_rebind_task_[big|lit|mid]\n");
	return 0x24;
}

/* add_uid_to_list 0010f6e4, .text+0x6cbc..0x7034. Signed upper check
 * admits negative values reinterpreted as u32, matching the original bug. */
void add_uid_to_list(u32 uid, int cluster)
{
	struct user_struct *user;

	if ((s32)uid >= 200000) {
		if (tsched_debug) printk("\0016metis-clu-aff: Big uid %d warning\n", uid);
		return;
	}
	user = find_user(KUIDT_INIT(uid));
	if (!user) return;
	if (!(is_32B_user(uid) & 1)) {
		bitmap_set_cluster(uid, cluster, cluster_affinity_uid_blist,
				   cluster_affinity_uid_mlist, cluster_affinity_uid_llist);
		if (tsched_debug) printk("\0016metis-clu-aff: Add uid %d to %d list\n", uid, cluster);
	}
	free_uid(user);
}

/* del_uid_from_list 0010f4f4, .text+0x6acc..0x6cbc. */
void del_uid_from_list(u32 uid, int cluster)
{
	if ((s32)uid >= 200000) {
		if (tsched_debug) printk("\0016metis-clu-aff: Big uid %d warning\n", uid);
		return;
	}
	bitmap_clear_cluster(uid, cluster, cluster_affinity_uid_blist,
			     cluster_affinity_uid_mlist, cluster_affinity_uid_llist);
	if (tsched_debug) printk("\0016metis-clu-aff: Del uid %d from %d list\n", uid, cluster);
}

/* get_clus_affinity_uidlist 0010f1c8, .text+0x67a0..0x6acc. */
int get_clus_affinity_uidlist(char *buffer, const struct kernel_param *param)
{
	printk("\0013metis-clu-aff: get_clus_affinity comm is : %s, pid is : %d\n", current->comm, current->pid);
	if (!control_allowed()) return 0;
	if (strstr(param->name, "show_lclus_affinity_uidlist"))
		return list_bitmap(buffer, cluster_affinity_uid_llist, 200000);
	if (strstr(param->name, "show_mclus_affinity_uidlist"))
		return list_bitmap(buffer, cluster_affinity_uid_mlist, 200000);
	if (strstr(param->name, "show_bclus_affinity_uidlist"))
		return list_bitmap(buffer, cluster_affinity_uid_blist, 200000);
	strcpy(buffer, "need show_[b|l|m]clus_affinity_uidlist\n");
	return 0x27;
}

/* set_clus_affinity_uidlist 0010ed28, .text+0x6300..0x679c. */
int set_clus_affinity_uidlist(const char *value, const struct kernel_param *param)
{
	const char *p, *end;
	u32 uid = 0;
	int cluster, colons, index;
	bool reset = false, remove = false;

	printk("\0013metis-clu-aff: set_clus_affinity comm is : %s, pid is : %d\n", current->comm, current->pid);
	if (!control_allowed()) return 0;
	if (strstr(param->name, "add_lclus_affinity_uidlist")) cluster = 1;
	else if (strstr(param->name, "add_mclus_affinity_uidlist")) cluster = 2;
	else if (strstr(param->name, "add_bclus_affinity_uidlist")) cluster = 3;
	else if (strstr(param->name, "reset_clus_affinity_uidlist")) { cluster = 0; reset = true; }
	else if (strstr(param->name, "del_lclus_affinity_uidlist")) { cluster = 1; remove = true; }
	else if (strstr(param->name, "del_mclus_affinity_uidlist")) { cluster = 2; remove = true; }
	else if (strstr(param->name, "del_bclus_affinity_uidlist")) { cluster = 3; remove = true; }
	else return 0;
	colons = colon_count(value);
	if (!colons) {
		if (sscanf(value, "%u\n", &uid) != 1) return -EINVAL;
		if (!reset) {
			if (remove) del_uid_from_list(uid, cluster);
			else add_uid_to_list(uid, cluster);
			return 0;
		}
		if (uid == 1) memset(cluster_affinity_uid_llist, 0, 25000);
		else if (uid == 3) memset(cluster_affinity_uid_blist, 0, 25000);
		else if (uid == 2) memset(cluster_affinity_uid_mlist, 0, 25000);
		else {
			memset(cluster_affinity_uid_blist, 0, 25000);
			memset(cluster_affinity_uid_mlist, 0, 25000);
			memset(cluster_affinity_uid_llist, 0, 25000);
		}
		return 0;
	}
	p = value;
	end = value + (int)strlen(value);
	for (index = 0; index < colons; index++) {
		if (sscanf(p, "%u", &uid) != 1) return -EINVAL;
		/* Stock multi-value reset follows the add branch, cluster0. */
		if (remove) del_uid_from_list(uid, cluster);
		else add_uid_to_list(uid, cluster);
		printk("\0016metis-clu-aff: arg %d val %d\n", index, uid);
		p = strchr(p + 1, ':') + 1;
		if (p >= end) return 0;
	}
	if (p < end) {
		if (sscanf(p, "%u", &uid) != 1) return -EINVAL;
		if (remove) del_uid_from_list(uid, cluster);
		else add_uid_to_list(uid, cluster);
		printk("\0016metis-clu-aff: arg %d val = %d\n", index, uid);
	}
	return 0;
}

/* uid_type 0010fa5c, .text+0x7034..0x70bc: word-index check allows
 * uid200000..200063 to index one word beyond the 25000-byte arrays. */
int uid_type(u32 uid)
{
	if ((uid >> 6) > 0xc34) return 0;
	if (test_bit(uid, cluster_affinity_uid_mlist)) return 2;
	if (test_bit(uid, cluster_affinity_uid_llist)) return 1;
	return test_bit(uid, cluster_affinity_uid_blist) ? 3 : 0;
}

/* proc_viptask_cpus_set 0010fae8, .text+0x70c0..0x71ac;
 * sysctl proc_handler's full five formals, CFI4d6f256d. */
int proc_viptask_cpus_set(struct ctl_table *table, int write, void *buffer,
			 size_t *lenp, loff_t *ppos)
{
	int result = proc_dointvec(table, write, buffer, lenp, ppos);
	u32 cpu;

	cpumask_bits(&viptask_use_cpumask)[0] = 0;
	for_each_possible_cpu(cpu) {
		if (cpu >= 32 || (s32)cpu >= vip_task_max_cpu_nr) break;
		set_bit(cpu, cpumask_bits(&viptask_use_cpumask));
		printk("\0016metis-clu-aff: vip task use max cpu nr %d\n", cpu);
	}
	return result;
}
