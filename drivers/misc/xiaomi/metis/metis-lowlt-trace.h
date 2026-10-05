/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM metis

#if !defined(_XIAOMI_METIS_LOWLT_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _XIAOMI_METIS_LOWLT_TRACE_H
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

/* Stock low-LT has three call sites but TWO events. Fields, array lengths,
 * signedness, print schemas and assignment paths are recovered from raw
 * stock event metadata/emission, not inferred from the call-site names.
 * Original parameter typedefs/qualifiers and native BPF/KCFI are UNTESTED. */
TRACE_EVENT(tracing_mark_write,
	TP_PROTO(char type, int tgid, const char *str, int cpu, int data),
	TP_ARGS(type, tgid, str, cpu, data),
	TP_STRUCT__entry(
		__field(char, type)
		__field(int, tgid)
		__array(char, str, 64 + 1)
		__field(int, str_data)
		__field(char, sep)
		__field(int, data)
	),
	TP_fast_assign(
		__entry->type = type;
		__entry->tgid = tgid;
		/* Stock reserves str_data but never assigns or prints it. */
		if (type == 'C') {
			snprintf(__entry->str, 64, "Cpu%d ", cpu);
			strlcat(__entry->str, str, 64 + 1);
			__entry->sep = '|';
		} else {
			/* Preserve the stock fixed 64-byte read, even for a short
			 * or empty input string. No source-length guard is added. */
			memcpy(__entry->str, str, 64);
			__entry->sep = ':';
		}
		__entry->str[64] = '\0';
		__entry->data = data;
	),
	TP_printk("%c|%d|%s%c%d", __entry->type, __entry->tgid,
		  __entry->str, __entry->sep, __entry->data)
);

TRACE_EVENT(mi_enqueue_vip_task,
	TP_PROTO(struct task_struct *task, const char *type),
	TP_ARGS(task, type),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__array(char, type, 64)
	),
	TP_fast_assign(
		/* Raw stock reads task+0x618 (pid), +0x830/+0x838 (comm).
		 * Use native task members; no task reference or comm lock. */
		__entry->pid = task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		strscpy(__entry->type, type, 64);
	),
	TP_printk("comm = %s pid = %d type = %s", __entry->comm,
		  __entry->pid, __entry->type)
);
#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE metis-lowlt-trace
#include <trace/define_trace.h>
