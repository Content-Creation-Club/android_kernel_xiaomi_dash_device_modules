// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: 33 stock cold-start control/classification/protection bodies.
 * Common VIP setters are shared with metis-common.c. State owners,
 * MM/scheduler producers and lifecycle remain absent. No registration,
 * exports, Makefile or replacement defaults. */
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/string.h>
#include "metis_coldstart.h"

/* Stock B.PL/B.MI tests the sign of wrapping 64-bit subtractions. */
static bool coldstart_window_active(void)
{
	return (s64)(coldstart_end_jif - coldstart_begin_jif) < 0 &&
	       (s64)(jiffies - coldstart_begin_jif - 1000UL) < 0;
}

static void coldstart_clear_auxiliary_data(void)
{
	home_tgid = 0;
	systemui_tgid = 0;
	systemserver_tgid = 0;
	sf_tgid = 0;
	cold_start_app_mmlock = NULL;
	cold_start_sserver_tgid = 0;
	cold_start_sserver_mmlock = NULL;
	home_mmlock = NULL;
	systemui_mmlock = NULL;
	systemserver_mmlock = NULL;
	sf_mmlock = NULL;
}

static void coldstart_clear_data(void)
{
	cold_start_app_tgid = 0;
	coldstart_clear_auxiliary_data();
}

/* .text+0x1ce50; stock task+0x61c/0x830 are tgid/comm. */
bool coldstart_app_ren_task(struct task_struct *task)
{
	if (!set_cold_start_render_vip || task->tgid != cold_start_app_tgid)
		return false;
	if (!task->tgid || !protect_cs)
		return false;
	if (!coldstart_window_active()) {
		coldstart_clear_data();
		return false;
	}
	return !strncmp(task->comm, "RenderThread", 16);
}

bool coldstart_app_tasks(struct task_struct *task)
{
	if (task->tgid != cold_start_app_tgid)
		return false;
	if (task->tgid && protect_cs) {
		if (coldstart_window_active())
			return true;
		coldstart_clear_data();
	}
	return false;
}

bool coldstart_app_ui_pid(int pid)
{
	if (cold_start_app_tgid != pid)
		return false;
	if (pid && protect_cs) {
		if (coldstart_window_active())
			return true;
		coldstart_clear_data();
	}
	return false;
}

bool coldstart_app_ui_task(struct task_struct *task)
{
	/* Stock +0x618 is pid, not tgid: only the process leader matches. */
	if (cold_start_app_tgid != task->pid)
		return false;
	if (task->pid && protect_cs) {
		if (coldstart_window_active())
			return true;
		coldstart_clear_data();
	}
	return false;
}

void coldstart_mark_mmlock_holder(struct task_struct *task)
{
	if (coldstart_set_mmlock_holder_vip)
		set_mi_vip_task_common_noresched(task, 0x08000000UL, 3UL);
}

bool coldstart_override_schedboost(struct task_struct *task)
{
	if (!override_schedboost_in_coldstart || !protect_cs ||
	    !cold_start_app_tgid)
		return false;
	if (!coldstart_window_active()) {
		coldstart_clear_data();
		return false;
	}
	if (task->tgid && task->tgid == cold_start_app_tgid) {
		/* The reference reads jiffies again after its initial window test. */
		if ((s64)(jiffies - coldstart_begin_jif - 1000UL) < 0)
			return false;
		coldstart_clear_data();
	}
	if (mi_vip_task(task))
		return false;
	return !mi_link_vip_task(task);
}

bool coldstart_sserver_tasks(struct task_struct *task)
{
	if (!protect_cs || !cold_start_app_tgid)
		return false;
	if (!coldstart_window_active()) {
		coldstart_clear_data();
		return false;
	}
	/* No extra nonzero check on the system-server identity in stock. */
	return task->tgid == cold_start_sserver_tgid;
}

void coldstart_tsk_ready_to_run(struct task_struct *task)
{
	struct mi_task_struct *state = get_mi_task_struct(task);
	void *mmlock;

	if (!state)
		return;
	mmlock = MI_FIELD(state, void *, 0x2d0);
	if (!mmlock)
		return;
	if ((sf_mmlock == mmlock || systemserver_mmlock == mmlock ||
	     systemui_mmlock == mmlock || home_mmlock == mmlock ||
	     cold_start_app_mmlock == mmlock) &&
	    coldstart_set_mmlock_holder_vip)
		set_mi_vip_task_common_noresched(task, 0x08000000UL, 2UL);
}

void coldstart_unmark_mmlock_holder(struct task_struct *task)
{
	if (coldstart_set_mmlock_holder_vip)
		clean_mi_vip_task_common(task, 0x08000000UL);
}

int get_coldstart_app_tgid(void)
{
	int tgid;

	if (!protect_cs || !cold_start_app_tgid)
		return -1;
	if ((s64)(coldstart_end_jif - coldstart_begin_jif) >= 0) {
		coldstart_clear_data();
		return -1;
	}
	tgid = cold_start_app_tgid;
	if ((s64)(jiffies - coldstart_begin_jif - 1000UL) >= 0) {
		coldstart_clear_data();
		return -1;
	}
	return tgid;
}

int get_protect_dur_us(void)
{
	return cs_protect_dur;
}

int get_sserver_tgid(char *buffer, const struct kernel_param *param)
{
	/* Both module-parameter getter arguments are ignored in stock. It
	 * returns the numeric TGID without writing the requested output buffer. */
	return cold_start_sserver_tgid;
}

bool is_important_mmlock(struct rw_semaphore *mmlock)
{
	/* Deliberately excludes cold_start_sserver_mmlock, as does stock. */
	return mmlock && (cold_start_app_mmlock == mmlock ||
		home_mmlock == mmlock || systemui_mmlock == mmlock ||
		systemserver_mmlock == mmlock || sf_mmlock == mmlock);
}

void protect_cs_task(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!protect_cs)
		return;
	state = get_mi_task_struct(task);
	if (!state)
		return;
	MI_FIELD(state, u32, 0x2b4) |= 8;
	MI_FIELD(state, s64, 0x2b8) = ktime_to_ns(ktime_get()) / 1000;
}

void protect_pre_cs_task(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!protect_cs)
		return;
	state = get_mi_task_struct(task);
	if (!state)
		return;
	MI_FIELD(state, u32, 0x2b4) |= 0x10;
	MI_FIELD(state, s64, 0x2b8) = ktime_to_ns(ktime_get()) / 1000;
}

int set_cold_start_app_tgid(int tid)
{
	struct task_struct *task;
	int lookup_tid;

	if (tid < 0) {
		pr_err("metis-cs: %s: set cold_start_app_tgid failed for tid<0: %d, cur %d %s\n",
		       __func__, tid, current->pid, current->comm);
		return -EINVAL;
	}
	rcu_read_lock();
	lookup_tid = tid;
	if (!tid) {
		lookup_tid = cold_start_app_tgid;
		if (!lookup_tid) {
			rcu_read_unlock();
			pr_err("metis-cs: %s: set cold_start_app_tgid failed for tid: %d, cur %d %s\n",
			       __func__, 0, current->pid, current->comm);
			return -EINVAL;
		}
	}
	task = pid_task(find_vpid(lookup_tid), PIDTYPE_PID);
	/* Preserve the stock separate zero test followed by refcount increment;
	 * this is intentionally not refcount_inc_not_zero(). */
	if (!task || !refcount_read(&task->usage)) {
		rcu_read_unlock();
		pr_err("metis-cs: %s: set cold_start_app_tgid failed: %d, cur %d %s\n",
		       __func__, tid, current->pid, current->comm);
		return -EINVAL;
	}
	get_task_struct(task);
	rcu_read_unlock();
	if (!tid) {
		coldstart_clear_data();
		stask_prefer_cluster = 0;
		coldstart_end_jif = jiffies;
		pr_info("metis-cs: %s: clean tgid %d, cur %d %s\n",
			__func__, task->tgid, current->pid, current->comm);
	} else {
		if (protect_cs && cold_start_app_tgid) {
			if (coldstart_window_active()) {
				pr_err("metis-cs: %s: already in coldstart, skip, previous %d, now %d\n",
				       __func__, cold_start_app_tgid, task->tgid);
				goto out_put;
			}
			/* Stock replaces the previous app identity without first zeroing
			 * it, but clears all eleven related identities/pointers. */
			coldstart_clear_auxiliary_data();
		}
		cold_start_app_tgid = task->tgid;
		stask_prefer_cluster = 2;
		coldstart_begin_jif = jiffies;
		pr_info("metis-cs: %s: set tgid %d for tsk %d %s\n",
			__func__, task->tgid, task->pid, task->comm);
	}
out_put:
	/* Match the stock decrement and immediate final put; don't add an
	 * invented workqueue/RCU retirement path to this recovered body. */
	if (refcount_dec_and_test(&task->usage))
		__put_task_struct(task);
	return 0;
}

int set_sserver_tgid(const char *buffer, const struct kernel_param *param)
{
	u32 tgid = 0;

	/* param_ops_sserver_tgid supplies the ignored second argument. */
	if (sscanf(buffer, "%u\n", &tgid) != 1)
		return -EINVAL;
	cold_start_sserver_tgid = (s32)tgid;
	if (cs_debug)
		pr_info("metis-cs: system server:tgid set success.pid:%d.\n", tgid);
	return 0;
}

void save_home_tgid(int tgid)
{
	home_tgid = tgid;
}

void save_systemui_tgid(int tgid)
{
	systemui_tgid = tgid;
}

void save_systemserver_tgid(int tgid)
{
	systemserver_tgid = tgid;
}

void save_sf_tgid(int tgid)
{
	sf_tgid = tgid;
}

int cold_start_ui_recent_used_cpu(void)
{
	if (metis_wakeup_enable && in_coldstart_period() &&
	    (s32)cold_start_ui_cpu >= 0 && cold_start_ui_cpu < nr_cpu_ids)
		return cold_start_ui_cpu;
	return -1;
}

int cold_start_ui_sched_state(void)
{
	if (metis_wakeup_enable && in_coldstart_period())
		return cold_start_ui_state;
	return 0;
}

int cold_start_ui_sleep(void)
{
	if (metis_wakeup_enable && in_coldstart_period())
		return cold_start_ui_state == 1;
	return false;
}

bool cold_start_ui_task(struct task_struct *task)
{
	bool active;

	if (!metis_wakeup_enable)
		return false;
	/* The expiry call precedes the task NULL check in the reference. */
	active = in_coldstart_period();
	return task && active && task->pid == cold_start_app_tgid;
}

bool cold_start_competing_with_ui(int cpu)
{
	int state = 0;
	unsigned int used_cpu = U32_MAX;

	if (metis_wakeup_enable) {
		if (in_coldstart_period())
			state = cold_start_ui_state;
		/* Two separate period calls/reloads occur in the stock body. */
		if (metis_wakeup_enable && in_coldstart_period() &&
		    (s32)cold_start_ui_cpu >= 0 && cold_start_ui_cpu < nr_cpu_ids)
			used_cpu = cold_start_ui_cpu;
	}
	return state > 1 && used_cpu == (u32)cpu;
}

bool cold_start_ind_task(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!metis_wakeup_enable || !in_coldstart_period())
		return false;
	state = get_mi_task_struct(task);
	return state && (state->unknown_c8 & 0x80);
}

bool cold_start_link_task(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!metis_wakeup_enable || !in_coldstart_period())
		return false;
	state = get_mi_task_struct(task);
	return state && (state->unknown_c8 & 0x2e0);
}

bool cold_start_ui_pipeline(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!metis_wakeup_enable || !in_coldstart_period())
		return false;
	state = get_mi_task_struct(task);
	return state && (state->flags_0xc0 & 0x200);
}

bool cold_start_wakeup_link_task(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!metis_wakeup_enable || !in_coldstart_period())
		return false;
	state = get_mi_task_struct(task);
	return state && (state->unknown_c8 & 0x60);
}

bool cold_start_wakeup_mark(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!metis_wakeup_enable || !in_coldstart_period())
		return false;
	state = get_mi_task_struct(task);
	return state && (state->unknown_c8 & 8);
}

bool cold_start_wakeup_task(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!metis_wakeup_enable || !in_coldstart_period())
		return false;
	state = get_mi_task_struct(task);
	return state && (state->unknown_c8 & 0xe0);
}

bool cold_start_wakeup_task_common(struct task_struct *task,
		unsigned long mask)
{
	struct mi_task_struct *state;

	if (!metis_wakeup_enable || !in_coldstart_period())
		return false;
	state = get_mi_task_struct(task);
	/* LDR w8 zero-extends state, while TST uses the full incoming x1. */
	return state && ((unsigned long)state->unknown_c8 & mask);
}
