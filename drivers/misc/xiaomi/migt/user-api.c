// SPDX-License-Identifier: GPL-2.0
/* Stock ioctl/proc/output bodies. Build, CFI and runtime remain untested. */
#include <linux/bug.h>
#include <linux/cred.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "user_api.h"
#include "../metis/metis_state.h"

/* .text+0x1f04..0x2714. Only the low byte of command is consumed. */
long migt_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	u32 uid = __kuid_val(current_uid()), count, threshold, quotient;
	u64 now;
	unsigned long tick;
	struct mi_task_struct *mi;
	int previous_pid, i, admitted, old_count;

	/* Stock performs a volatile jiffies read even on the disabled path. */
	(void)jiffies;
	if (!boost_policy)
		return 0;
	if (migt_debug)
		printk("\0013migt: migt boost comming. uid %d %d %d %llu", uid, 837,
			command & 0xff, (u64)(ktime_to_ns(ktime_get()) / 1000));
	switch (command & 0xff) {
	case 1:
		uid = __kuid_val(current_uid());
		if (uid != last_traced_uid || uid != get_foreground_uid()) {
			if (ceiling_freq_limit_work == 1) {
				cancel_delayed_work_sync(&ceiling_rem);
				ceiling_next_update = jiffies;
				queue_delayed_work_on(WORK_CPU_UNBOUND, migt_wq, &ceiling_rem, 1);
			}
			reset_render_info(0);
			reset_render_info(1);
			sys_migt_count = 0;
			if (boost_policy == 3)
				game_load_reset();
			force_reset_runtime = 1;
			last_traced_uid = uid;
			show_traced_uid = uid;
			if (boost_policy)
				set_foreground_app(uid);
		}
		update_render_info(current, 0);
		if (!migt_enabled || !boost_policy)
			return 0;
		threshold = migt_thresh;
		mi = get_mi_task_struct(current);
		if (mi)
			MI_FIELD(mi, u64, 0x278) = jiffies;
		sys_migt_count = (u32)sys_migt_count + 1;
		if ((u32)sys_migt_count > first_lunch_fps && (u32)boost_policy >= 2) {
			count = sys_migt_count;
			quotient = itask_detect_interval ? count / itask_detect_interval : 0;
			previous_pid = render_pid;
			if (count - quotient * itask_detect_interval == 0 ||
			    current->pid != previous_pid) {
				glk_freq_limit_start = 1;
				if (current->pid != previous_pid) {
					if (migt_debug)
						printk("\0016migt: render pid change from %d to %d\n",
							previous_pid, current->pid);
					if (!render_update)
						goto frame_detected;
				}
				add_work_to_migt(__kuid_val(current_uid()));
			}
		}
frame_detected:
		if ((u32)sys_migt_count <= first_lunch_fps)
			glk_freq_limit_start = 0;
		render_pid = current->pid;
		now = fas_sched_clock();
		if (now - migt_last_update_time >= (u64)(u32)migt_thresh * 1000000)
			over_thresh_count++;
		migt_boost_flag = false;
		boost_cycle = 0;
		migt_last_update_time = now;
		if (ceiling_freq_limit_work == 1 &&
		    (long)(ceiling_next_update - jiffies) < 0) {
			tick = jiffies;
			ceiling_next_update = tick + msecs_to_jiffies(ceiling_slack_ms >> 1);
			cancel_delayed_work_sync(&ceiling_rem);
			queue_delayed_work_on(WORK_CPU_UNBOUND, migt_wq, &ceiling_rem,
				msecs_to_jiffies(ceiling_slack_ms));
		}
		switch (boost_policy) {
		case 0:
		case 2:
			break;
		case 3:
			/* Stock passes the generation in w0; the existing body ignores it. */
			game_load_history_update();
			if (ceiling_enable == 1)
				queue_delayed_work_on(WORK_CPU_UNBOUND, migt_wq, &ceiling_work, 1);
			threshold = (u32)migt_thresh + ((u32)migt_thresh >> 1);
			fallthrough;
		case 1:
			if (!high_resolution_enable) {
				cancel_delayed_work_sync(&migt_work);
				queue_delayed_work_on(WORK_CPU_UNBOUND, migt_wq, &migt_work,
					msecs_to_jiffies(threshold));
			} else {
				if (hrtimer_active(&migt_frame_timer))
					hrtimer_cancel(&migt_frame_timer);
				hrtimer_start_range_ns(&migt_frame_timer, (s64)(s32)threshold * 1000000,
					0, HRTIMER_MODE_REL);
			}
			break;
		default:
			printk("\0013migt: invalid version number %d\n", boost_policy);
		}
		return 0;
	case 3:
		update_render_info(current, 1);
		return 0;
	case 4:
		old_count = ceiling_users_count;
		count = old_count < 10 ? old_count : 10;
		uid = last_traced_uid;
		ceiling_enable = 1;
		if (migt_debug)
			printk("--------ceiling_user list--------\n");
		admitted = old_count > 0 ? max((s32)count, 1) : 0;
		if (migt_debug && old_count > 0) {
			for (i = 0; i < admitted; ++i)
				printk("%d ", ceiling_users[i]);
		}
		for (i = 0; i < admitted; ++i)
			if (ceiling_users[i] == uid)
				return 0;
		if (migt_debug)
			printk("\nadd uid %d to ceiling users\n", uid);
		if (old_count >= 10) {
			/* A full list shifts left and replaces its last UID without growing. */
			for (i = 0; i < 9; ++i)
				ceiling_users[i] = ceiling_users[i + 1];
			ceiling_users[9] = uid;
			return 0;
		}
		BUG_ON((u32)admitted > 9);
		ceiling_users[admitted] = uid;
		ceiling_users_count = (u32)ceiling_users_count + 1;
		return 0;
	case 8:
		if (uid != last_ui_traced_uid) {
			if (boost_policy == 2)
				frame_load_reset();
			last_ui_traced_uid = uid;
		}
	}
	return 0;
}

/* .text+0x2718..0x2720: literal success without creating any mapping. */
int migt_mmap(struct file *file, struct vm_area_struct *vma) { return 0; }
/* .text+0x2724..0x272c and0x2730..0x2738: literal stock empty callbacks. */
int migt_open(struct inode *inode, struct file *file) { return 0; }
int migt_release(struct inode *inode, struct file *file) { return 0; }

/* .text+0xf8c..0xf94: stock accepts and discards parameter writes. */
int exec_buckets_set(const char *value, const struct kernel_param *param) { return 0; }

static u32 real_uid(struct task_struct *task)
{
	u32 uid;
	rcu_read_lock();
	uid = __kuid_val(rcu_dereference(task->real_cred)->uid);
	rcu_read_unlock();
	return uid;
}

static const char *game_class(struct task_struct *task)
{
	if (game_super_task(task)) return "sup ";
	if (is_render_thread(task)) return "ren ";
	if (is_uuser_set_vip(task) & 1) return "uvip";
	if (is_uuser_set_ip(task) & 1) return "uip ";
	if (game_vip_task(task)) return "vip ";
	return game_ip_task(task) ? "imp " : "nor ";
}

/* .text+0xf98..0x1170: native process/thread lists replace all old task offsets. */
int exec_buckets_get(char *buffer, const struct kernel_param *param)
{
	struct task_struct *process, *task;
	struct migt_task_state *gt;
	const char *classification;
	int used = 0;
	rcu_read_lock();
	for_each_process(process) {
		if (real_uid(process) != show_traced_uid)
			continue;
		for_each_thread(process, task) {
			gt = get_gt_task(task);
			if (!gt || !(mi_vip_task(task) & 1))
				continue;
			used += snprintf(buffer + used, 4096 - (long)used,
				"%5d,%16s ", task->pid, task->comm);
			classification = game_class(task);
			used += snprintf(buffer + used, 4096 - (long)used, "%s %c\n",
				classification, mi_vip_task(task) & 1 ? 'v' : 'c');
		}
	}
	rcu_read_unlock();
	return used;
}

/* .text+0x1c88..0x1f00: unlike the parameter getter, show includes non-VIP tasks. */
int exec_buckets_show(struct seq_file *seq, void *unused)
{
	struct task_struct *process, *task;
	struct migt_task_state *gt;
	const char *classification;
	u32 uid = *(u32 *)seq->private;
	int i;
	rcu_read_lock();
	for_each_process(process) {
		if (real_uid(process) != uid)
			continue;
		seq_printf(seq, "traced_uid: %d\n", uid);
		for_each_thread(process, task) {
			gt = get_gt_task(task);
			if (!gt)
				continue;
			seq_printf(seq, "%5d,%16s buckests:{", task->pid, task->comm);
			for (i = 0; i < 10; ++i)
				seq_printf(seq, " %8d", GT_FIELD(gt, u32, 0x90 + i * 4));
			classification = game_class(task);
			seq_printf(seq, "  } %s %c\n", classification,
				mi_vip_task(task) & 1 ? 'v' : 'c');
		}
	}
	rcu_read_unlock();
	return 0;
}

/* .text+0x1c38..0x1c84: private points to a live UID, not an open-time copy. */
int exec_buckets_open(struct inode *inode, struct file *file)
{
	int ret = single_open(file, exec_buckets_show, NULL);
	if (!ret)
		((struct seq_file *)file->private_data)->private = &show_traced_uid;
	return ret;
}

const struct proc_ops migt_exec_buckets_proc_ops = {
	.proc_open = exec_buckets_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* .text+0x559c..0x55b4: 16-byte sort record, unsigned descending score. */
int cmp(const void *left, const void *right)
{
	u64 a = ((const struct migt_rank_entry *)left)->score;
	u64 b = ((const struct migt_rank_entry *)right)->score;
	return a > b ? -1 : a < b;
}

/* .text+0x7484..0x7618: signed remainder then unsigned fatal index guard. */
void dump_his_load_info(int index, struct seq_file *seq)
{
	u32 slot = index % 21;
	struct migt_game_history *record;
	int cluster;
	static const char cluster_names[3] = { 'l', 'm', 'b' };
	BUG_ON(slot > 20);
	record = &game_load_history[slot];
	if (!record->period)
		return;
	seq_printf(seq, "period %llu: \n", record->period);
	for (cluster = 0; cluster < 3 && migt_game_cluster_number > cluster; ++cluster)
		seq_printf(seq, "%c: %llu %lu\t\n", cluster_names[cluster],
			record->load[cluster], MI_FIELD(record->raw_28, unsigned long, cluster * 8));
	BUG_ON(cluster == 3 && migt_game_cluster_number > 3);
	seq_printf(seq, "\n");
}

/* Distinct duplicate static names: frame0x6198..0x61b8 and0x61ec..0x620c. */
int migt_frame_history_load_show(struct seq_file *seq, void *unused)
{
	migt_enable();
	return 0;
}
int migt_frame_cpu_load_show(struct seq_file *seq, void *unused)
{
	migt_enable();
	return 0;
}

/* Original frame open wrappers0x6168..0x6194 and0x61bc..0x61e8. */
int migt_frame_history_load_open(struct inode *inode, struct file *file)
{
	return single_open(file, migt_frame_history_load_show, NULL);
}
int migt_frame_cpu_load_open(struct inode *inode, struct file *file)
{
	return single_open(file, migt_frame_cpu_load_show, NULL);
}

/* Original game history show0x7408..0x7484. Print tail, then head, inclusive. */
int migt_game_history_load_show(struct seq_file *seq, void *unused)
{
	s32 first = cur_history_items, next = (u32)first + 1;
	if (next < 21) {
		do {
			dump_his_load_info(next, seq);
			next = (u32)next + 1;
		} while (next != 21);
		if (first < 0)
			return 0;
	}
	next = 0;
	do {
		dump_his_load_info(next, seq);
		next = (u32)next + 1;
	} while (next <= first);
	return 0;
}

static void show_game_counter(struct seq_file *seq, u64 *counter)
{
	u64 divisor = cpu_util_freq_divisor;
	if (!divisor) {
		cpu_util_freq_divisor = 1;
		divisor = 1;
	}
	seq_printf(seq, "%9lu \t", (unsigned long)(*counter / divisor));
}

/* Original game CPU show0x764c..0x7904. Reader mutates a zero global divisor. */
int migt_game_cpu_load_show(struct seq_file *seq, void *unused)
{
	u32 index = cur_history_items ? cur_history_items - 1 : 20, mask;
	unsigned int cpu;
	int cluster;
	BUG_ON(index > 20);
	seq_printf(seq, "Period :%d\n", (u32)game_load_history[index].period);
	for (cluster = 0; cluster < migt_game_cluster_number; ++cluster) {
		seq_printf(seq, "---cluster %d--- \n", cluster);
		BUG_ON(cluster == 3);
		mask = (u32)migt_game_cluster_cpumask[cluster];
		while (mask) {
			struct migt_game_cpu *info;
			cpu = __ffs(mask);
			mask &= ~(1U << cpu);
			info = &per_cpu(migt_game_load_info, cpu);
			show_game_counter(seq, &info->all_last);
			show_game_counter(seq, &info->game_last);
			show_game_counter(seq, &info->super_last);
			show_game_counter(seq, &info->vip_last);
			show_game_counter(seq, &info->render_last);
			show_game_counter(seq, &info->counter_58);
			show_game_counter(seq, &info->counter_68);
		}
		seq_printf(seq, "\n");
	}
	return 0;
}

/* Original game open wrappers0x73d8..0x7404 and0x761c..0x7648. */
int migt_game_history_load_open(struct inode *inode, struct file *file)
{
	return single_open(file, migt_game_history_load_show, NULL);
}
int migt_game_cpu_load_open(struct inode *inode, struct file *file)
{
	return single_open(file, migt_game_cpu_load_show, NULL);
}

const struct proc_ops migt_frame_history_proc_ops = {
	.proc_open = migt_frame_history_load_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};
const struct proc_ops migt_frame_cpu_proc_ops = {
	.proc_open = migt_frame_cpu_load_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};
static const struct proc_ops game_history_proc_ops = {
	.proc_open = migt_game_history_load_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};
static const struct proc_ops game_cpu_proc_ops = {
	.proc_open = migt_game_cpu_load_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};
static struct proc_dir_entry *game_load_dir, *game_history_entry, *game_cpu_entry;

/* .text+0x6ad0..0x6b58: retain partial creation and absence of error return. */
void create_game_load_proc(struct proc_dir_entry *parent)
{
	if (!parent)
		return;
	game_load_dir = proc_mkdir("glk", parent);
	if (!game_load_dir)
		return;
	game_history_entry = proc_create("game_history_load", 0664, game_load_dir,
		&game_history_proc_ops);
	game_cpu_entry = proc_create("game_cpu_load", 0664, game_load_dir, &game_cpu_proc_ops);
}

/* .text+0x6b5c..0x6bb0: stock leaves all three pointers stale after removal. */
void delete_game_load_proc(void)
{
	if (!game_load_dir)
		return;
	if (game_cpu_entry)
		proc_remove(game_cpu_entry);
	if (game_history_entry)
		proc_remove(game_history_entry);
	proc_remove(game_load_dir);
}

/* Stock native preinitialized mutex .data+0x300. */
static DEFINE_MUTEX(proc_glk_dointvec_mutex);

/* .text+0x6bb4..0x6d20: six arguments, optional native five-argument handler. */
int proc_glk_dointvec(struct ctl_table *table, int write, void *buffer,
		size_t *length, loff_t *position, proc_handler *handler)
{
	void *data = table->data, *saved;
	int ret;
	mutex_lock(&proc_glk_dointvec_mutex);
	if (table->maxlen != 12)
		table->maxlen = 12;
	if (!handler)
		handler = proc_dointvec;
	if (!write) {
		ret = handler(table, 0, buffer, length, position);
	} else {
		/* Original flags0xdc0 include __GFP_ZERO, not just GFP_KERNEL. */
		saved = kzalloc(12, GFP_KERNEL);
		if (!saved) {
			ret = -ENOMEM;
		} else {
			BUG_ON((u32)table->maxlen > 12);
			memcpy(saved, data, table->maxlen);
			ret = handler(table, write, buffer, length, position);
			if (ret) {
				BUG_ON((u32)table->maxlen > 12);
				memcpy(data, saved, table->maxlen);
			}
			kfree(saved);
		}
	}
	mutex_unlock(&proc_glk_dointvec_mutex);
	return ret;
}

/* .text+0x7908..0x7a94. Shared transaction, then live debug reads after unlock. */
int proc_glk_dotload(struct ctl_table *table, int write, void *buffer,
		size_t *length, loff_t *position)
{
	int ret = proc_glk_dointvec(table, write, buffer, length, position, NULL);
	int cluster;
	if (game_load_debug)
		for (cluster = 0; cluster < 3; ++cluster)
			printk("\0016glk: cid %d minload %d maxload %d\n", cluster,
				glk_lowspeed_load[cluster], glk_highspeed_load[cluster]);
	return ret;
}

/* .text+0x7a98..0x7c24. Shares only the recovered transaction helper. */
int proc_glk_dofreq(struct ctl_table *table, int write, void *buffer,
		size_t *length, loff_t *position)
{
	int ret = proc_glk_dointvec(table, write, buffer, length, position, NULL);
	int cluster;
	if (game_load_debug)
		for (cluster = 0; cluster < 3; ++cluster)
			printk("\0016glk: cid %d minfreq %d maxfreq %d\n", cluster,
				glk_minfreq[cluster], glk_maxfreq[cluster]);
	return ret;
}

/* .text+0x33c8..0x33cc, CFI0xa540670c: literal stock void(void) no-op. */
void trigger_ceiling_limit(void) {}
