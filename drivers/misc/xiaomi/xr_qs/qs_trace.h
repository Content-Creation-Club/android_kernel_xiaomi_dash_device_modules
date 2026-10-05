/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM qos_sched_events

#if !defined(_XIAOMI_QS_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _XIAOMI_QS_TRACE_H
#include <linux/sched.h>
#include <linux/tracepoint.h>

/* Fields, order and formats recovered from stock trace_event_fields_* and
 * trace_event_raw_event_*; prio is the internal scheduler priority. */
TRACE_EVENT(qs_set_rt_sched,
	TP_PROTO(struct task_struct *task, int prio, bool sched_rr,
		 bool inherit_prio_fork, int ret),
	TP_ARGS(task, prio, sched_rr, inherit_prio_fork, ret),
	TP_STRUCT__entry(__field(pid_t, pid) __field(int, prio)
		__field(bool, sched_rr) __field(bool, inherit_prio_fork)
		__field(int, ret)),
	TP_fast_assign(__entry->pid = task->pid; __entry->prio = prio;
		__entry->sched_rr = sched_rr;
		__entry->inherit_prio_fork = inherit_prio_fork; __entry->ret = ret;),
	TP_printk("qos set rt sched: pid=%d prio=%d, rr=%d, inherit_prio=%d, ret=%d",
		__entry->pid, __entry->prio, __entry->sched_rr,
		__entry->inherit_prio_fork, __entry->ret)
);

TRACE_EVENT(qs_restore_rt_sched,
	TP_PROTO(struct task_struct *task), TP_ARGS(task),
	TP_STRUCT__entry(__field(pid_t, pid)),
	TP_fast_assign(__entry->pid = task->pid;),
	TP_printk("qos restore rt sched: pid=%d", __entry->pid)
);

TRACE_EVENT(qs_correct_rt_sched,
	TP_PROTO(struct task_struct *task, int prio, int ret),
	TP_ARGS(task, prio, ret),
	TP_STRUCT__entry(__field(pid_t, pid) __field(int, prio) __field(int, ret)),
	TP_fast_assign(__entry->pid = task->pid; __entry->prio = prio;
		__entry->ret = ret;),
	TP_printk("qos correct rt sched: pid=%d prio=%d ret=%d",
		__entry->pid, __entry->prio, __entry->ret)
);

TRACE_EVENT(qs_binder_preset,
	TP_PROTO(struct task_struct *task), TP_ARGS(task),
	TP_STRUCT__entry(__field(pid_t, pid)),
	TP_fast_assign(__entry->pid = task->pid;),
	TP_printk("qos sched binder preset: pid=%d", __entry->pid)
);

TRACE_EVENT(sl_set_mi_vip,
	TP_PROTO(struct task_struct *task, int prio, unsigned long sem),
	TP_ARGS(task, prio, sem),
	TP_STRUCT__entry(__field(pid_t, pid) __field(int, prio)
		__field(unsigned long, sem)),
	TP_fast_assign(__entry->pid = task->pid; __entry->prio = prio;
		__entry->sem = sem;),
	TP_printk("speedlock set: pid=%d, vip_prio=%d, sem=%lu",
		__entry->pid, __entry->prio, __entry->sem)
);

TRACE_EVENT(sl_reset_mi_vip,
	TP_PROTO(struct task_struct *task, unsigned long sem), TP_ARGS(task, sem),
	TP_STRUCT__entry(__field(pid_t, pid) __field(unsigned long, sem)),
	TP_fast_assign(__entry->pid = task->pid; __entry->sem = sem;),
	TP_printk("speedlock reset: pid=%d, sem=%lu", __entry->pid, __entry->sem)
);

TRACE_EVENT(sl_timeout,
	TP_PROTO(struct task_struct *task), TP_ARGS(task),
	TP_STRUCT__entry(__field(pid_t, pid)),
	TP_fast_assign(__entry->pid = task->pid;),
	TP_printk("speedlock timeout: pid=%d", __entry->pid)
);

TRACE_EVENT(qs_rt_fork_inherit,
	TP_PROTO(struct task_struct *from, struct task_struct *to),
	TP_ARGS(from, to),
	TP_STRUCT__entry(__field(pid_t, from_pid) __field(pid_t, to_pid)),
	TP_fast_assign(__entry->from_pid = from->pid; __entry->to_pid = to->pid;),
	TP_printk("%d fork inherit prio from %d", __entry->to_pid, __entry->from_pid)
);
#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE qs_trace
#include <trace/define_trace.h>
