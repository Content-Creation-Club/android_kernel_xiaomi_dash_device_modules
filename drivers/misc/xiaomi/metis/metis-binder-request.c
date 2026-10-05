// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: stock asynchronous Binder request state, without hook wiring. */
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include "metis_binder_request.h"

void set_mi_req_async_binder_link_task(struct task_struct *task, u32 ticks)
{
	struct mi_task_struct *state;
	unsigned long deadline;
	unsigned int debug;

	if (!task)
		return;
	state = get_mi_task_struct(task);
	if (!state)
		return;
	deadline = jiffies + ticks;
	/* B.PL tests the wrapped subtraction sign, not signed relational order. */
	if ((s64)(mi_link_boost_jif - deadline) < 0)
		mi_link_boost_jif = deadline;
	if ((s64)(state->deadline_0x290 - deadline) < 0)
		state->deadline_0x290 = deadline;
	debug = metis_debug;
	state->flags_0xc0 |= 0x10000;
	if (debug)
		pr_info("metis-fas: set async binder link task flag, pid = %d, name = %s",
			task->pid, task->comm);
}

void clean_mi_req_async_binder_link_task(struct task_struct *task)
{
	struct mi_task_struct *state;
	unsigned int debug;

	if (!task)
		return;
	state = get_mi_task_struct(task);
	if (!state)
		return;
	debug = metis_debug;
	state->flags_0xc0 &= 0xfffeffffU;
	if (debug)
		pr_info("metis-fas: clear async binder link task flag, pid = %d, name = %s",
			task->pid, task->comm);
}

void mi_req_async_binder_link_sched_start(struct task_struct *task, u32 milliseconds)
{
	/* .text+0xe410 MUL w8 wraps before division; literal 250 and cap12. */
	u32 ticks = (milliseconds * 250U) / 1000U;

	if (ticks > 11)
		ticks = 12;
	set_mi_req_async_binder_link_task(task, ticks);
}

void mi_req_async_binder_link_sched_end(struct task_struct *task)
{
	clean_mi_req_async_binder_link_task(task);
}
