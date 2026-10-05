/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM metis
#if !defined(_XIAOMI_METIS_COMMON_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _XIAOMI_METIS_COMMON_TRACE_H
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

/* Recovered from stock descriptors/raw event stores, not helper names. */
TRACE_EVENT(metis_set_vip_task,
	TP_PROTO(struct task_struct *task, const char *flag, int resched, int jiff),
	TP_ARGS(task, flag, resched, jiff),
	TP_STRUCT__entry(
		__field(int, pid)
		__array(char, flag, 10)
		__field(int, resched)
		__field(int, jiff)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		/* Stock copies ten bytes regardless of input length and does
		 * not terminate this array. Keep the overread/truncation. */
		memcpy(__entry->flag, flag, 10);
		__entry->resched = resched;
		__entry->jiff = jiff;
	),
	TP_printk("pid=%d vip_flag=%s resched=%d jiff=%d", __entry->pid,
		  __entry->flag, __entry->resched, __entry->jiff)
);

TRACE_EVENT(mi_link_vip_task,
	TP_PROTO(struct task_struct *task, const char *type),
	TP_ARGS(task, type),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__array(char, type, 64)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		strscpy(__entry->type, type, 64);
	),
	TP_printk("comm = %s pid = %d type = %s", __entry->comm,
		  __entry->pid, __entry->type)
);

TRACE_EVENT(mi_dynamic_vip_task,
	TP_PROTO(struct task_struct *task, unsigned int start, unsigned int end,
		 const char *type),
	TP_ARGS(task, start, end, type),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(unsigned int, start)
		__field(unsigned int, end)
		__array(char, comm, TASK_COMM_LEN)
		__array(char, type, 64)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		__entry->start = start;
		__entry->end = end;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		strscpy(__entry->type, type, 64);
	),
	TP_printk("comm = %s pid = %d start jif = %d end jif = %d type = %s",
		  __entry->comm, __entry->pid, __entry->start, __entry->end,
		  __entry->type)
);

TRACE_EVENT(metis_set_dynamic_vip,
	TP_PROTO(struct task_struct *task, int set, int jiff),
	TP_ARGS(task, set, jiff),
	TP_STRUCT__entry(
		__field(int, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(int, set)
		__field(int, jiff)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->set = set;
		__entry->jiff = jiff;
	),
	TP_printk("pid=%d comm=%s is_set=%d jiff=%d", __entry->pid,
		  __entry->comm, __entry->set, __entry->jiff)
);
#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE metis-common-trace
#include <trace/define_trace.h>
