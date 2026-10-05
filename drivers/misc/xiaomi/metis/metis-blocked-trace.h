/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM metis
#if !defined(_XIAOMI_METIS_BLOCKED_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _XIAOMI_METIS_BLOCKED_TRACE_H
#include <linux/types.h>
#include <linux/tracepoint.h>
/* Stock .data+0xac8 field table: pid_t owner_pid (4/signed), char owner_type
 * (1/unsigned), bool is_reader, bool spinnable, bool is_vip, then sentinel.
 * Raw event stores at entry+8,+c,+d,+e,+f and reserves 16 bytes. */
TRACE_EVENT(mi_rwsem_blocked_info,
	TP_PROTO(pid_t owner_pid, char owner_type, bool is_reader,
		 bool spinnable, bool is_vip),
	TP_ARGS(owner_pid, owner_type, is_reader, spinnable, is_vip),
	TP_STRUCT__entry(
		__field(pid_t, owner_pid)
		__field(char, owner_type)
		__field(bool, is_reader)
		__field(bool, spinnable)
		__field(bool, is_vip)
	),
	TP_fast_assign(
		__entry->owner_pid = owner_pid;
		__entry->owner_type = owner_type;
		__entry->is_reader = is_reader;
		__entry->spinnable = spinnable;
		__entry->is_vip = is_vip;
	),
	TP_printk("[vip %d] tsk is blocked by rwsem, owner %d type %c, cur is reader %d spinnable %d",
		  __entry->is_vip, __entry->owner_pid, __entry->owner_type,
		  __entry->is_reader, __entry->spinnable)
);
#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE metis-blocked-trace
#include <trace/define_trace.h>
