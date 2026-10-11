// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL stock VIP/link state paths. No registration or module lifecycle. */
#include <linux/cred.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/rwsem.h>
#include "kernel/sched/sched.h"
#include "metis_common.h"
#include "metis_blocked.h"
#include "metis_coldstart.h"
#define CREATE_TRACE_POINTS
#include "metis-common-trace.h"

static void metis_link_message(struct task_struct *task, unsigned long mask,
			      bool set)
{
	char message[64] = { 0 };

	if (!trace_mi_link_vip_task_enabled())
		return;
	snprintf(message, sizeof(message), set ? "set %s vip" : "clear %s vip",
		 mi_link_mask_to_type(mask));
	trace_mi_link_vip_task(task, message);
}

void mi_resched_vip_task(struct task_struct *task)
{
	struct rq_flags flags = { 0 };
	struct rq *rq;

	if (!task)
		return;
	rq = task_rq_lock(task, &flags);
	update_rq_clock(rq);
	if (rq->curr != task && task->on_rq == TASK_ON_RQ_QUEUED) {
		/* Both stock calls receive literal 10; no policy normalization. */
		deactivate_task(rq, task, 10);
		activate_task(rq, task, 10);
	}
	/* Stock's unpin copy and unlock expand to this native helper. */
	task_rq_unlock(rq, task, &flags);
}

void set_mi_vip_task_common_noresched(struct task_struct *task,
				    unsigned long mask, unsigned long duration)
{
	struct mi_task_struct *state;
	unsigned long deadline;

	if (!task || !(state = get_mi_task_struct(task)))
		return;
	deadline = jiffies + duration;
	if (deadline > state->deadline_0x290)
		state->deadline_0x290 = deadline;
	state->flags_0xc0 |= (u32)mask;
	trace_metis_set_vip_task(task, mi_link_mask_to_type(mask), 0, (int)duration);
	if (link_debug)
		pr_info("metis-link: metis link: set %s vip task flag noresched, pid = %d, name = %s",
			mi_link_mask_to_type(mask), task->pid, task->comm);
	metis_link_message(task, mask, true);
}

void set_mi_vip_task_common(struct task_struct *task, unsigned long mask,
			  unsigned long duration)
{
	struct mi_task_struct *state;
	struct task_struct *owner;
	struct rw_semaphore *sem;
	struct mutex *lock;
	unsigned long deadline, word;
	u32 blocked;

	/* Stock excludes policies 1 and 2 by this wrapping unsigned comparison. */
	if (!task || task->policy - 3U >= 0xfffffffeU ||
	    !(state = get_mi_task_struct(task)))
		return;
	rcu_read_lock();
	deadline = jiffies + duration;
	if (deadline > state->deadline_0x290)
		state->deadline_0x290 = deadline;
	state->flags_0xc0 |= (u32)mask;
	rcu_read_unlock();
	metis_link_message(task, mask, true);
	mi_resched_vip_task(task);
	trace_metis_set_vip_task(task, mi_link_mask_to_type(mask), 1, (int)duration);
	if (link_debug)
		pr_info("metis-link: metis link: set %s vip task flag, pid = %d, name = %s",
			mi_link_mask_to_type(mask), task->pid, task->comm);
	blocked = MI_FIELD(state, u32, 0x2b4);
	if (!(blocked & 0x140))
		return;
	if (duration > 15)
		duration = 15;
	if (duration < 6)
		return;
	sem = MI_FIELD(state, struct rw_semaphore *, 0x2d0);
	if (sem) {
		word = atomic_long_read(&sem->owner);
		if ((blocked & 0x40) && !(word & 1) && (word & ~3UL)) {
			owner = (struct task_struct *)(word & ~3UL);
			set_mi_vip_task_common(owner, 0x2000, duration - 5);
			if (link_debug)
				pr_warn("metis-link: %s: tsk %d %s try to set %d %s as vip for rwsem writer\n",
					__func__, task->pid, task->comm, owner->pid,
					owner->comm);
		}
		/* Stock marks sem even if no eligible writer was boosted. */
		set_rwsem_vip_blocked(sem);
		return;
	}
	lock = MI_FIELD(state, struct mutex *, 0x2d8);
	if (!lock)
		return;
	word = atomic_long_read(&lock->owner);
	if (!(blocked & 0x100) || !(word & ~7UL))
		return;
	owner = (struct task_struct *)(word & ~7UL);
	set_mi_vip_task_common(owner, 0x2000, duration - 5);
	set_mutex_vip_blocked(lock);
	if (link_debug)
		pr_warn("metis-link: %s: tsk %d %s try to set %d %s as vip for mutex\n",
			__func__, task->pid, task->comm, owner->pid, owner->comm);
}

void clean_mi_vip_task_common(struct task_struct *task, unsigned long mask)
{
	struct mi_task_struct *state;

	if (!task || !(state = get_mi_task_struct(task)))
		return;
	rcu_read_lock();
	state->flags_0xc0 &= ~(u32)mask;
	rcu_read_unlock();
	if (link_debug)
		pr_info("metis-link: metis link: clear %s vip task flag, pid = %d, name = %s",
			mi_link_mask_to_type(mask), task->pid, task->comm);
	metis_link_message(task, mask, false);
}

static bool metis_expiring_link(struct task_struct *task, u32 mask)
{
	struct mi_task_struct *state;

	if (!task || !(state = get_mi_task_struct(task)))
		return false;
	if ((s64)((u64)jiffies - state->deadline_0x290) < 0)
		return !!(state->flags_0xc0 & mask);
	if (state->flags_0xc0 & mask)
		clean_mi_vip_task_common(task, mask);
	return false;
}

bool mi_lock_vip_task(struct task_struct *task)
{
	return metis_expiring_link(task, 0x2000);
}

bool mi_binder_vip_task(struct task_struct *task)
{
	return metis_expiring_link(task, 0x4000);
}

bool mi_art_mutex_vip_task(struct task_struct *task)
{
	return metis_expiring_link(task, 0x20000);
}

void set_mi_vip_task(struct task_struct *task, u32 duration, unsigned long mask)
{
	struct mi_task_struct *state;
	unsigned long deadline;
	unsigned int debug;

	if (!task || !(state = get_mi_task_struct(task)))
		return;
	if (duration) {
		deadline = jiffies + duration;
		if (deadline > state->deadline_0x290)
			state->deadline_0x290 = deadline;
	}
	debug = metis_debug;
	if ((mask & 0x100000) && !(state->flags_0xc0 & 0x100000))
		atomic_inc(&mi_dynamic_vip_num);
	if ((mask & 0x400000) && !(state->flags_0xc0 & 0x400000))
		atomic_inc(&mi_sdk_eboost_vip_num);
	state->flags_0xc0 |= (u32)mask | 0x2000000;
	if (debug && mask == 0x400000)
		pr_info("metis-fas: set mi eboost vip task: cnt = %d, pid = %d, name = %s",
			atomic_read(&mi_sdk_eboost_vip_num), task->pid, task->comm);
}

s32 mi_task_uid(struct mi_task_struct *state)
{
	s32 uid;

	if (!state)
		return -1;
	uid = (s32)state->uid;
	if (uid < 0) {
		rcu_read_lock();
		uid = __kuid_val(__task_cred(state->task)->uid);
		rcu_read_unlock();
		state->uid = (u32)uid;
	}
	return uid;
}

void set_mi_dynamic_vip_task(struct task_struct *task, u32 duration)
{
	struct mi_task_struct *state;
	unsigned long deadline, start, end;

	if (!task || !(state = get_mi_task_struct(task)))
		return;
	deadline = jiffies + duration;
	if ((s64)(mi_link_boost_jif - deadline) < 0)
		mi_link_boost_jif = deadline;
	if ((s64)(state->deadline_0x290 - deadline) < 0)
		state->deadline_0x290 = deadline;
	end = state->deadline_0x290;
	if ((s64)(MI_FIELD(state, u64, 0x278) - deadline) < 0)
		MI_FIELD(state, u64, 0x278) = deadline;
	state->flags_0xc0 |= 0x800;
	start = jiffies;
	trace_mi_dynamic_vip_task(task, (u32)start, (u32)end, "set dynamic vip");
	if (metis_debug)
		pr_info("metis-fas: set dynamic vip task flag, pid = %d, name = %s \n",
			task->pid, task->comm);
	hook_dynamic_vip_setting(task, 1);
	trace_metis_set_dynamic_vip(task, 1, (int)duration);
}

void clean_mi_dynamic_vip_task(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!task || !(state = get_mi_task_struct(task)))
		return;
	state->flags_0xc0 &= ~0x800U;
	trace_mi_dynamic_vip_task(task, 0, 0, "clean dynamic vip");
	if (metis_debug)
		pr_info("metis-fas: clear dynamic vip task flag, pid = %d, name = %s \n",
			task->pid, task->comm);
	trace_metis_set_dynamic_vip(task, 0, 0);
}

bool mi_dynamic_vip_task(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	if (!state)
		return false;
	if (task_maybe_notify_thread(task) || coldstart_app_ren_task(task))
		return true;
	if (!(state->flags_0xc0 & 0x800))
		return false;
	if ((s64)((u64)jiffies - state->deadline_0x290) < 0 ||
	    is_top_app_vip(state) || is_static_vip(state) ||
	    (s64)((u64)jiffies - MI_FIELD(state, u64, 0x278)) < 0)
		return true;
	clean_mi_dynamic_vip_task(task);
	return false;
}

bool task_maybe_notify_thread(struct task_struct *task)
{
	if (!protect_cs || !set_possible_notifythr_vip)
		return false;
	/* No NULL guard or task ref is present in stock. Expiry resets the
	 * global identities only after this matching, nonzero TGID gate. */
	if (!task->tgid || task->tgid != cold_start_app_tgid)
		return false;
	if (!in_coldstart_period())
		return false;
	if (task->pid != last_ui_forked_pid)
		return false;
	return cold_start_ui_futex_wait ||
		(s64)(last_ui_forked_jif - jiffies + 1UL) >= 0;
}

void hook_dynamic_vip_setting(struct task_struct *task, bool set)
{
	const char *leader;
	int tgid;

	if (!task)
		return;
	tgid = set ? task->tgid : 0;
	if (!strcmp(task->comm, "com.miui.home"))
		home_tgid = tgid;
	/* Stock assumes group_leader exists and keeps its unlocked comm pointer. */
	leader = task->group_leader->comm;
	if (!strcmp(leader, "ndroid.systemui"))
		systemui_tgid = tgid;
	if (!strcmp(leader, "system_server"))
		systemserver_tgid = tgid;
	if (!strcmp(leader, "surfaceflinger"))
		sf_tgid = tgid;
}

void mi_dynamic_vtask_prio_sched_start(struct task_struct *task, int milliseconds)
{
	u32 ticks = ((u32)milliseconds * 250U) / 1000U;

	if (ticks < 26)
		ticks = 25;
	if (ticks > 1249)
		ticks = 1250;
	if (task && metis_debug)
		pr_info("metis-fas: metis ioctl: set dynamic vip, pid=%d time = %d, jiff = %d \n",
			task->pid, milliseconds, ticks);
	set_mi_dynamic_vip_task(task, ticks);
	mi_resched_vip_task(task);
}

void mi_dynamic_vtask_prio_sched_end(struct task_struct *task)
{
	clean_mi_dynamic_vip_task(task);
}

void mi_binder_restore_prio_sched_start(struct task_struct *task, int milliseconds)
{
	u32 ticks = (u32)milliseconds * 250U > 1999U ? 2 : 1;

	if (task && metis_debug)
		pr_info("metis-fas: metis ioctl: set dynamic vip, pid=%d time = %d, jiff = %d \n",
			task->pid, milliseconds, ticks);
	set_mi_dynamic_vip_task(task, ticks);
	mi_resched_vip_task(task);
}

void metis_clean_vip_thread(struct task_struct *task)
{
	struct mi_task_struct *state, *owner_state;
	struct task_struct *owner;
	u32 mask;

	if (!task || !(state = get_mi_task_struct(task)))
		return;
	if (state->flags_0xc0 & 0x1000)
		state->flags_0xc0 &= ~0x1000U;
	if (!(state->flags_0xc0 & 0x80)) {
		owner = state->task;
		owner_state = get_mi_task_struct(owner);
		if (owner_state && (owner_state->flags_0xc0 & 0x20)) {
			if (metis_debug)
				pr_info("metis-fas: Task %s %d req to clean vip flag\n",
					owner->comm, owner->pid);
			state->flags_0xc0 |= 0x80;
		}
	}
	mask = mi_dynamic_vip_task(task) ? 0xfe7ffaff : 0xfe7ff2ff;
	state->flags_0xc0 &= mask;
}

bool metis_xxx_task(struct task_struct *task, unsigned long mask)
{
	unsigned int cpu = task_cpu(task), cluster;
	struct mi_task_struct *state = get_mi_task_struct(task), *again;
	u32 flags;
	bool foreign;
	s32 uid;

	if (!state)
		return false;
	flags = state->flags_0xc0;
	/* Keep the second real lookup rather than treating the accessor as pure. */
	again = get_mi_task_struct(task);
	if (flags & 0x2000000)
		return again && (again->flags_0xc0 & mask);
	foreign = true;
	if (again) {
		uid = mi_task_uid(again);
		foreign = !foreground_uid || (u32)uid != foreground_uid;
	}
	if (!(mask & 0x800) && foreign)
		return false;
	if (!(state->flags_0xc0 & mask))
		return false;
	if ((s64)((u64)jiffies - MI_FIELD(state, u64, 0x278)) < 0)
		return true;
	cluster = cpu_to_cluster(cpu);
	if (cluster >= 3) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	perf_min_freq[cluster] = 0;
	MI_FIELD(state, u64, 0x278) = 0;
	metis_clean_vip_thread(task);
	return false;
}

bool metis_vip_task(struct task_struct *task)
{
	return metis_xxx_task(task, 0xd00);
}

bool metis_ren_task(struct task_struct *task)
{
	return metis_xxx_task(task, 0x400);
}

bool metis_sup_task(struct task_struct *task)
{
	return coldstart_app_ui_task(task) || metis_xxx_task(task, 0x100);
}

bool metis_ip_task(struct task_struct *task)
{
	return metis_xxx_task(task, 0x10);
}

bool metis_vvip_task(struct task_struct *task)
{
	struct mi_task_struct *state;
	u32 mask = 0x21c00000;
	s32 uid;

	if (mi_dynamic_vip_task(task))
		return true;
	state = get_mi_task_struct(task);
	if (state) {
		uid = mi_task_uid(state);
		if (foreground_uid && (u32)uid == foreground_uid)
			mask = 0x21c00020;
	}
	state = get_mi_task_struct(task);
	return state && (state->flags_0xc0 & mask);
}

bool metis_ui_task_quick(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	return state && (state->flags_0xc0 & 0x100);
}

bool metis_ui_pipeline(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	return state && (state->flags_0xc0 & 0x200);
}

void metis_set_ui_task(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	if (state)
		state->flags_0xc0 |= 0x100;
}

u32 get_process_config_index(struct task_struct *task)
{
	struct mi_task_struct *leader;

	if (!task)
		return UINT_MAX;
	leader = get_mi_task_struct(task->group_leader);
	return leader ? MI_FIELD(leader, u32, 0x2fc) : UINT_MAX;
}

u8 *get_important_task_info(struct mi_task_struct *state)
{
	u32 index;

	if (!state)
		return NULL;
	index = MI_FIELD(state, u32, 0x2f8);
	if (index == UINT_MAX)
		index = get_process_config_index(state->task);
	return index < 7 ? important_tasks[index].raw : NULL;
}

s32 get_metis_deadline_ms(struct mi_task_struct *state)
{
	struct mi_task_struct *leader;
	u8 *info = get_important_task_info(state);
	u32 policy;

	if (!info)
		return 0;
	policy = MI_FIELD(info, u32, 0x18);
	if (policy & 8)
		return MI_FIELD(info, s32, 0x1c);
	if (!(policy & 16) || !state->task || !state->task->group_leader)
		return 0;
	leader = get_mi_task_struct(state->task->group_leader);
	if (leader && (leader->flags_0xc0 & 0x800))
		return MI_FIELD(info, s32, 0x1c);
	return 0;
}

u32 get_preempt_prio(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!task)
		return 0;
	if (cold_start_ui_task(task) || coldstart_app_ui_task(task))
		return 5;
	if (cold_start_link_task(task))
		return 4;
	if (mi_vip_task(task) || mi_link_vip_task(task))
		return 3;
	if (task_in_cs_protect(task))
		return 2;
	state = get_mi_task_struct(task);
	return state && get_metis_deadline_ms(state) > 0 ? 1 : 0;
}

bool is_uuser_set_vip(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	return state && (state->flags_0xc0 & 0x2c0ae020);
}

bool is_uuser_set_ip(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	return state && (state->flags_0xc0 & 2);
}
