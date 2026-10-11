// SPDX-License-Identifier: GPL-2.0
/* Stock worker/policy source recovery; see WORKER_CONTRACT.txt. */
#include <linux/bug.h>
#include <linux/kthread.h>
#include <linux/refcount.h>
#include <linux/sched/task.h>
#include <linux/sched/user.h>
#include <linux/sort.h>
#include <linux/string.h>
#include "kernel/sched/sched.h"
#include "worker.h"
#include "../metis/metis_state.h"

/* .text+0x183c..0x188c. Wake accounting precedes the existing VIP result. */
void find_energy_hook(struct task_struct *task, bool *is_vip)
{
	wake_render(task);
	*is_vip = *is_vip ? true : game_vip_task(task) != 0;
}

/* .text+0x1890..0x19f4. The same metis slot proves ignored arg4 is int. */
void walt_get_indicies_hook(struct task_struct *task, u32 *cluster,
		int *start_index, int unused_reason, bool *handled)
{
	u32 choice;
	bool result = true;

	if (!boost_policy)
		return;
	if (force_cluster_sched_enable)
		*start_index = 0;
	if (game_super_task(task)) {
		if (stask_prefer_cluster < 0) {
			*cluster = 1;
		} else {
			choice = stask_prefer_cluster;
			if (choice > 2) {
				choice = 2;
				stask_prefer_cluster = 2;
			}
			*cluster = choice;
			*start_index = 0;
		}
	} else if (game_stask_pipeline(task) && ui_run_cpu >= 0) {
		*cluster = get_cluster_type(ui_run_cpu);
	} else if (is_render_thread(task)) {
		if (render_prefer_cluster < 0) {
			*cluster = 1;
		} else {
			*cluster = render_prefer_cluster;
			*start_index = 0;
		}
	} else if (game_render_pipeline(task) && render_run_cpu >= 0) {
		*cluster = get_cluster_type(render_run_cpu);
	} else if (game_vip_task(task)) {
		if (vip_prefer_cluster >= 0) {
			*cluster = vip_prefer_cluster;
		} else {
			*cluster = 1;
			if (render_run_cpu >= 0) {
				choice = get_cluster_type(render_run_cpu);
				*cluster = choice == 2 ? 1 : choice;
			}
		}
	} else if (game_ip_task(task) && ip_prefer_cluster >= 0) {
		*cluster = ip_prefer_cluster;
	} else {
		result = false;
	}
	*handled = result;
}

/* .text+0x424c..0x465c. CPU31 is a stock sentinel, not a native CPU count. */
bool game_lb_control(struct task_struct *task, int source_cpu,
		int destination_cpu, int unused_capacity_delta)
{
	struct mi_task_struct *mi;
	struct migt_task_state *gt;
	struct rq *rq;
	u64 now, stamp;
	unsigned long super;
	unsigned int render;

	BUG_ON((u32)source_cpu >= 32);
	rq = cpu_rq(source_cpu);
	/* Stock performs the provider lookup before its task NULL check. */
	mi = get_mi_task_struct(task);
	if (!migt_enable() || !task || !mi || !force_forbidden_walt_lb)
		return false;
	if (rq_deadloop_detect(rq))
		return false;
	now = fas_sched_clock();
	stamp = MI_FIELD(mi, u64, 0x280);
	/* Stock task+0x108 is se.on_rq, as in metis coldstart balance policy. */
	if (stamp && task->se.on_rq &&
	    now - stamp > (u64)task_enqueue_time_toolong * 1000000ULL)
		return false;
	super = game_super_task(task);
	if (destination_cpu == 31 && !super &&
	    stask_prefer_cluster == 2)
		return true;
	render = is_render_thread(task);
	if (destination_cpu == 31 && !render &&
	    render_prefer_cluster == 2)
		return true;
	if (game_super_task(task) && stask_prefer_cluster >= 0 &&
	    get_cluster_type(destination_cpu) != stask_prefer_cluster &&
	    get_cluster_type(source_cpu) == stask_prefer_cluster)
		return true;
	if (is_render_thread(task) && render_prefer_cluster >= 0 &&
	    get_cluster_type(destination_cpu) != render_prefer_cluster &&
	    get_cluster_type(source_cpu) == render_prefer_cluster)
		return true;
	/* The stock module parameter is uint, but this comparison is signed. */
	if ((s32)force_forbidden_walt_lb < 2) {
		if (migt_is_uuser_set_vip(task) && vip_prefer_cluster >= 0 &&
		    get_cluster_type(destination_cpu) != vip_prefer_cluster &&
		    get_cluster_type(source_cpu) == vip_prefer_cluster)
			return true;
		if (!migt_is_uuser_set_ip(task))
			return false;
	} else {
		if (game_vvip_task(task) && vip_prefer_cluster >= 0 &&
		    get_cluster_type(destination_cpu) != vip_prefer_cluster &&
		    get_cluster_type(source_cpu) == vip_prefer_cluster)
			return true;
		if (!game_task(task))
			return false;
		gt = get_gt_task(task);
		if (!gt || !(GT_FLAGS(gt) & 0x1fc))
			return false;
	}
	return ip_prefer_cluster >= 0 &&
	       get_cluster_type(destination_cpu) != ip_prefer_cluster &&
	       get_cluster_type(source_cpu) == ip_prefer_cluster;
}

/* Stock render_clain_flw .text+0x5de4..0x5f34. */
void migt_render_link_flw(struct task_struct *task, unsigned int wake_flags)
{
	struct mi_task_struct *mi;
	struct migt_task_state *gt;
	struct task_struct *waker;

	if (!migt_enable() || frame_load_has_init != 1 || !flw_enable)
		return;
	if (migt_enable()) {
		mi = get_mi_task_struct(task);
		if (mi && !mi->uid) {
			gt = get_gt_task(task);
			if (flw_debug == 12 && game_vip_task(task))
				/* Stock does not guard gt after the second lookup. */
				printk("\0016flw: %s set wake %llu\n", task->comm,
				       GT_FIELD(gt, u64, 0x58));
		}
	}
	waker = current;
	if (!is_render_thread(waker)) {
		if (!migt_enable() || frame_load_has_init != 1 || !flw_enable)
			return;
		gt = get_gt_task(waker);
		if (!gt || (GT_FLAGS(gt) & 0xc00) != 0x400)
			return;
	}
	if (!(wake_flags & 0x10) || (waker->flags & PF_EXITING))
		return;
	if (is_rcal_task(waker, 0) & 1) {
		set_flw_flag(waker, 11, 0);
		set_flw_flag(task, 10, 11);
	}
	mi = get_mi_task_struct(task);
	if (mi)
		mi_set_fas_req(mi);
}

/* Other stock render_clain_flw .text+0x6238..0x629c, game-link consumer. */
void migt_frame_link_flw(struct task_struct *task, unsigned int wake_flags)
{
	struct task_struct *waker;

	if (!(frame_load_enable() & 1))
		return;
	waker = current;
	if ((is_rcal_task(waker, 0) & 1) && (wake_flags & 0x10) &&
	    !(waker->flags & PF_EXITING))
		game_wake_task(task);
}

/* Extraction helpers below are parts of migt_hardwork, not extra bodies. */
static void worker_snapshot_render(unsigned int index)
{
	struct migt_render_info *info = &render_info[index];
	u8 *back = &rinfo_back[index * 0x40];
	unsigned int sequence;

	back[8] = 0;
	do {
		sequence = read_seqbegin(&info->lock);
		back[8] = info->valid;
		memcpy(back + 0x18, info->comm, TASK_COMM_LEN);
	} while (read_seqretry(&info->lock, sequence));
}

static struct migt_task_state *worker_candidate(unsigned int index)
{
	return (struct migt_task_state *)(unsigned long)user_array[index].unknown_00;
}

static struct mi_task_struct *worker_mi(struct migt_task_state *gt)
{
	return (struct mi_task_struct *)((u8 *)gt - 0x1a0);
}

static struct task_struct *worker_owner(struct migt_task_state *gt)
{
	struct mi_task_struct *mi = worker_mi(gt);

	return mi ? mi->task : NULL;
}

static bool worker_reset_runtime(void)
{
	return (need_reset_runtime_everytime | (u32)force_reset_runtime) != 0;
}

static void worker_clear_roles(struct migt_task_state *gt)
{
	struct task_struct *task = worker_owner(gt);
	struct mi_task_struct *mi;

	if (!task || !is_render_thread(task)) {
		if (GT_FLAGS(gt) & 0x100) {
			mi = get_mi_task_struct(task);
			if (mi)
				mi_clean_fas_req(mi);
		}
		GT_FLAGS(gt) &= 0xfffffe29U;
	} else if (!set_render_as_stask) {
		GT_FLAGS(gt) &= ~0x100U;
	}
}

static void worker_request_fas(struct task_struct *task)
{
	struct mi_task_struct *mi = get_mi_task_struct(task);

	if (mi)
		mi_set_fas_req(mi);
}

static unsigned int worker_select_super(unsigned int count)
{
	s32 limit;
	u64 max_load = 0, max_exec = 0;
	unsigned int load_index = 0, exec_index = 0, i;
	struct migt_task_state *gt;
	u64 load, exec;

	if (pick_stask_policy != 1)
		return 0;
	limit = min((s32)stask_candidate_num, (s32)count);
	for (i = 0; (s64)i < limit; i++) {
		BUG_ON(i >= 1024);
		gt = worker_candidate(i);
		/* Stock has no NULL-candidate guard in this selection pass. */
		load = GT_FIELD(gt, u64, 0x40);
		exec = GT_FIELD(gt, u64, 0x30);
		if (load > max_load) {
			max_load = load;
			load_index = i;
		}
		if (exec > max_exec) {
			max_exec = exec;
			exec_index = i;
		}
	}
	if (migt_render_debug)
		printk("\0016migt-gtc: max %d mindex %d, max_r %llu, m_exec %llu, V: %d\n",
		       load_index, exec_index, max_exec, max_load, 50);
	return max_load >= 50 ? load_index : exec_index;
}

static bool worker_classify_user(struct mi_user_struct *user_state,
		struct user_struct *user)
{
	struct list_head held, *node, *next;
	struct mi_task_struct *entry, *mi;
	struct task_struct *task;
	struct migt_task_state *gt;
	unsigned int count = 0, selected, i;
	s32 vip_limit, ip_limit;

	worker_snapshot_render(0);
	worker_snapshot_render(1);
	read_lock_irq(&user_state->lock);
	if (list_empty(&user_state->tasks)) {
		read_unlock_irq(&user_state->lock);
		/* This branch does not consume force_reset_runtime. */
		return false;
	}
	INIT_LIST_HEAD(&held);
	for (node = user_state->tasks.next; node != &user_state->tasks;
	     node = node->next) {
		entry = list_entry(node, struct mi_task_struct, user_node);
		if (!entry || !entry->task)
			break;
		task = entry->task;
		if (!refcount_inc_not_zero(&task->usage))
			continue;
		/* Stock takes a second reference and drops it without a free test. */
		get_task_struct(task);
		refcount_dec(&task->usage);
		mi = get_mi_task_struct(task);
		gt = mi ? (struct migt_task_state *)((u8 *)mi + 0x1a0) : NULL;
		if (gt && (s32)entry->uid >= 0 && entry->uid == __kuid_val(user->uid)) {
			if (worker_reset_runtime()) {
				GT_FIELD(gt, u64, 0xb0) = 0;
				GT_FIELD(gt, u64, 0xa8) = 0;
				GT_FIELD(gt, u64, 0xa0) = 0;
				GT_FIELD(gt, u64, 0x98) = 0;
				GT_FIELD(gt, u64, 0x90) = 0;
			}
			if (is_render_thread(task) || is_frender_sibling(task)) {
				if (is_frender_sibling(task))
					worker_clear_roles(gt);
				if (worker_reset_runtime())
					GT_FIELD(gt, u64, 0x40) = 0;
				if (!set_render_as_stask) {
					GT_FLAGS(gt) |= 0x10;
				} else {
					GT_FLAGS(gt) |= 0x100;
					worker_request_fas(mi->task);
				}
			} else if (GT_FIELD(gt, u32, 8)) {
				BUG_ON(count >= 1024);
				user_array[count].unknown_00 = (unsigned long)gt;
				user_array[count].score = GT_FIELD(gt, u32, 8);
				if (worker_reset_runtime())
					GT_FIELD(gt, u64, 0x40) = 0;
				/* Stock writes slot1023 then excludes it from the sort/count. */
				if (count == 1023) {
					printk("\0013migt-gtc: too many VIP task\n");
					break;
				}
				count++;
				continue;
			} else {
				worker_clear_roles(gt);
				if (worker_reset_runtime())
					GT_FIELD(gt, u64, 0x40) = 0;
			}
		}
		/* This node belongs to the original user-list entry, not the lookup. */
		list_add((struct list_head *)entry->unknown_100, &held);
	}
	read_unlock_irq(&user_state->lock);
	list_for_each_safe(node, next, &held) {
		entry = (struct mi_task_struct *)((u8 *)node - 0x100);
		if (!entry)
			continue;
		list_del(node);
		/* Re-read provider owner; stock does not retain the original pointer. */
		task = entry->task;
		if (task)
			put_task_struct(task);
	}
	if (!count)
		return true;
	sort(user_array, count, sizeof(user_array[0]), cmp, NULL);
	selected = worker_select_super(count);
	for (i = 0; i < count; i++) {
		BUG_ON(i >= 1024);
		gt = worker_candidate(i);
		if (!gt) {
			printk("\0013migt-gtc: %d: task is NULL\n", i);
			/* Stock skips the release and table clear in this case. */
			continue;
		}
		if (i < 20 && migt_render_debug) {
			task = worker_owner(gt);
			/* Stock also has no task NULL check on this diagnostic path. */
			printk("\0013migt-gtc: %3d:comm %16s pid %6d key:%llu load %llu est %llu sup:%d\n",
			       i, task->comm, task->pid, user_array[i].score,
			       GT_FIELD(gt, u64, 0x40), GT_FIELD(gt, u64, 0x30), selected);
			gt = worker_candidate(i);
		}
		worker_clear_roles(gt);
		gt = worker_candidate(i);
		task = worker_owner(gt);
		if (i == selected) {
			GT_FLAGS(gt) |= 0x100;
			worker_request_fas(task);
		} else {
			/* Native signed overflow is avoided; stock adds in w registers. */
			vip_limit = (s32)(stask_candidate_num + vip_task_max_num);
			if ((s64)i < vip_limit) {
				GT_FLAGS(gt) |= 0x10;
			} else {
				ip_limit = (s32)((u32)vip_limit + ip_task_max_num);
				if ((s64)i < ip_limit)
					GT_FLAGS(gt) |= 4;
			}
		}
		if (worker_reset_runtime())
			GT_FIELD(worker_candidate(i), u32, 8) = 0;
		/* Deliberately no NULL guard and no saved original task reference. */
		put_task_struct(worker_owner(worker_candidate(i)));
		user_array[i].unknown_00 = 0;
		user_array[i].score = 0;
	}
	return true;
}

/* .text+0x35bc..0x404c. uid_work is the real u32 work item given to kthread. */
int migt_hardwork(void *uid_work)
{
	u32 *work_uid = uid_work;
	struct user_struct *user;
	struct mi_user_struct *state;

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!migt_has_work_todo) {
			schedule();
			/* Stop is checked only after the no-work sleep, as in stock. */
			if (kthread_should_stop()) {
				printk("\0016migt-gtc: migt_ca has exit\n");
				return 0;
			}
		}
		set_current_state(TASK_RUNNING);
		if (*work_uid) {
			/* Stock passes the stored UID word directly, without namespace mapping. */
			user = find_user(KUIDT_INIT(*work_uid));
			if (user) {
				state = get_mi_user_struct(user);
				if (state && worker_classify_user(state, user))
					force_reset_runtime = 0;
				free_uid(user);
			}
			*work_uid = 0;
		}
		migt_has_work_todo = 0;
	}
}
