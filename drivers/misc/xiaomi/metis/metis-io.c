// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: four stock IO bodies; subsystem helpers are not invoked here. */
#include <linux/kallsyms.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/workqueue.h>
#include <trace/events/workqueue.h>
#include "metis_common.h"
#include "metis_io.h"

/* Native declaration also exists in linux/fortify-string.h when enabled. */
extern void fortify_panic(const char *name) __noreturn __cold;

/* Original event schemas/backing tracepoint owners remain unreconstructed. */
DECLARE_TRACE(metis_iowait_status_update,
	TP_PROTO(struct task_struct *task, u32 flags), TP_ARGS(task, flags));
DECLARE_TRACE(metis_verity_work_start,
	TP_PROTO(struct task_struct *task, u64 elapsed, int window),
	TP_ARGS(task, elapsed, window));

void metis_io_schedule_hook(struct task_struct *task)
{
	struct mi_task_struct *state;

	if (!task)
		return;
	state = get_mi_task_struct(task);
	if (!state || !metis_vip_task(task) || !task->in_iowait)
		return;
	vip_iowait_time = (u64)(ktime_to_ns(ktime_get()) / 1000000);
	/* This producer deliberately has no metis_io_enabled gate. */
	if (trace_metis_iowait_status_update_enabled())
		trace_metis_iowait_status_update(task, MI_FIELD(state, u32, 0xc0));
}

void probe_workqueue_execute_start(void *unused, struct work_struct *work)
{
	char symbol[666];
	u64 elapsed;
	size_t length;
	int comparison;

	if (!work || !metis_io_enabled)
		return;
	memset(symbol, 0, sizeof(symbol));
	elapsed = (u64)(ktime_to_ns(ktime_get()) / 1000000) - vip_iowait_time;
	if (elapsed > 4)
		return;
	sprint_symbol(symbol, (unsigned long)work->func);
	length = strnlen(VERITY_WORK_NAME, sizeof(VERITY_WORK_NAME));
	if (length > sizeof(VERITY_WORK_NAME))
		fortify_panic("strnlen");
	if (length == sizeof(VERITY_WORK_NAME))
		fortify_panic("__fortify_strlen");
	comparison = strncmp(symbol, VERITY_WORK_NAME, length);
	if (!length || comparison)
		return;
	trace_metis_verity_work_start(current, elapsed, 4);
	set_mi_vip_task_common_noresched(current, 0x4000000UL, 1UL);
}

void metis_io_init(void)
{
	/* Original ignores tracepoint_probe_register's return value. */
	register_trace_workqueue_execute_start(probe_workqueue_execute_start, NULL);
}

void metis_io_exit(void)
{
	/* Stock .text+0x20408..0x2040c is a literal RET: no unregister. */
}
