// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: four stock RT/balance policy bodies; no provider integration. */
#include <linux/bug.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include "kernel/sched/sched.h"
#include "metis_coldstart_policy.h"

static void coldstart_policy_expire(void)
{
	cold_start_app_tgid = 0;
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

bool metis_coldstart_skip_thiscpu_rt(struct task_struct *task, int cpu)
{
	if (metis_wakeup_cold_start_skip_ui_cpu(task, cpu))
		return true;
	if ((s32)stask_prefer_cpu >= 1 && stask_prefer_cpu != (u32)cpu)
		return false;
	if (!protect_cs || !cold_start_app_tgid)
		return false;
	if (!in_coldstart_period())
		return false;
	if (!task->tgid || task->tgid != cold_start_app_tgid)
		return true;
	if ((s64)(jiffies - coldstart_begin_jif - 1000UL) < 0)
		return false;
	/* The second age check can expire, yet the stock saved result is true. */
	coldstart_policy_expire();
	return true;
}

bool metis_coldstart_spread_rt_task(struct task_struct *task)
{
	if (!protect_cs || !cold_start_app_tgid)
		return false;
	if (!in_coldstart_period())
		return false;
	if ((u32)(stask_prefer_cpu - 8) <= 0xfffffff8U)
		return false;
	if (!task->pid || task->pid != cold_start_app_tgid)
		return true;
	if ((s64)(jiffies - coldstart_begin_jif - 1000UL) >= 0) {
		coldstart_policy_expire();
		return true;
	}
	return false;
}

bool metis_coldstart_skip_lb(struct task_struct *task, int source_cpu,
		int destination_cpu, int capacity_delta)
{
	struct mi_task_struct *state;
	u32 preferred;
	int pid;
	u64 now, stamp;

	if (metis_wakeup_cold_start_skip_ui_cpu(task, destination_cpu))
		return true;
	if (!protect_cs || !cold_start_app_tgid || !in_coldstart_period())
		return false;
	preferred = stask_prefer_cpu;
	if ((u32)(preferred - 8) < 0xfffffff9U)
		return false;
	if (!task)
		return preferred == (u32)destination_cpu;
	pid = task->pid;
	if (pid && pid == cold_start_app_tgid) {
		if ((s64)(jiffies - coldstart_begin_jif - 1000UL) < 0)
			goto ui_task;
		coldstart_policy_expire();
	}
	if (preferred == (u32)destination_cpu)
		return true;
ui_task:
	if (!pid || pid != cold_start_app_tgid)
		return false;
	if ((s64)(jiffies - coldstart_begin_jif - 1000UL) >= 0) {
		coldstart_policy_expire();
		return false;
	}
	if (preferred == (u32)destination_cpu || preferred != (u32)source_cpu)
		return false;
	BUG_ON((u32)source_cpu > 31);
	state = get_mi_task_struct(task);
	if (!state || rq_deadloop_detect(cpu_rq(source_cpu)))
		return false;
	now = fas_sched_clock();
	stamp = MI_FIELD(state, u64, 0x280);
	if (!stamp)
		return true;
	/* Stock se base task+0xc0, vruntime+0x128; +0x108 is se.on_rq.
	 * exec_start is the following aligned u64 at +0x110. */
	return !task->se.on_rq || now - stamp < 0x05f5e101ULL;
}

void metis_coldstart_sched_balance_rt(void *unused, struct rq *rq,
		struct task_struct *task, int *done)
{
	if (metis_wakeup_cold_start_skip_ui_cpu(task, rq->cpu))
		*done = 1;
	if (protect_cs && cold_start_app_tgid && in_coldstart_period() &&
	    (s32)stask_prefer_cpu > 0 && stask_prefer_cpu == rq->cpu)
		*done = 1;
}
