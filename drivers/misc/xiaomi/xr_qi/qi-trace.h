/* SPDX-License-Identifier: GPL-2.0 */
/* Trace field types and formats recovered from stock .data relocations. */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM qos_inherit_events
#if !defined(_QI_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _QI_TRACE_H
#include <linux/tracepoint.h>
#include <linux/sched.h>
#include <linux/string.h>

TRACE_EVENT(qi_binder_proc_transaction,
	TP_PROTO(int ret, struct task_struct *from, struct task_struct *to, u32 static_qos, u32 dyn_qos),
	TP_ARGS(ret, from, to, static_qos, dyn_qos),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(pid_t, from)
		__field(pid_t, to)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->from = from->pid;
		__entry->to = to->pid;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
	),
	TP_printk("binder proc transaction: ret=%d from=%d to=%d static_qos=%u dyn_qos=%u", __entry->ret, __entry->from, __entry->to, __entry->static_qos, __entry->dyn_qos)
);

TRACE_EVENT(qi_binder_restore_priority,
	TP_PROTO(int ret, void * trans, struct task_struct *pid, u32 static_qos, u32 dyn_qos),
	TP_ARGS(ret, trans, pid, static_qos, dyn_qos),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(void *, trans)
		__field(pid_t, pid)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->trans = trans;
		__entry->pid = pid->pid;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
	),
	TP_printk("binder restore priority: ret=%d trans=%p pid=%d static_qos=%u dyn_qos=%u", __entry->ret, __entry->trans, __entry->pid, __entry->static_qos, __entry->dyn_qos)
);

TRACE_EVENT(qi_binder_set_priority,
	TP_PROTO(int ret, void * trans, struct task_struct *from, struct task_struct *to, u32 static_qos, u32 dyn_qos),
	TP_ARGS(ret, trans, from, to, static_qos, dyn_qos),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(void *, trans)
		__field(pid_t, from)
		__field(pid_t, to)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->trans = trans;
		__entry->from = from->pid;
		__entry->to = to->pid;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
	),
	TP_printk("binder set priority: ret=%d tasns=%p from=%d to=%d static_qos=%u dyn_qos=%u", __entry->ret, __entry->trans, __entry->from, __entry->to, __entry->static_qos, __entry->dyn_qos)
);

TRACE_EVENT(qi_free_task,
	TP_PROTO(struct task_struct *pid, int flag, u32 static_qos, u32 dyn_qos, unsigned long timeout),
	TP_ARGS(pid, flag, static_qos, dyn_qos, timeout),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(int, flag)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
		__field(unsigned long, timeout)
	),
	TP_fast_assign(
		__entry->pid = pid->pid;
		__entry->flag = flag;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
		__entry->timeout = timeout;
	),
	TP_printk("free task: pid=%d, falg=%d, static_qos=%u, dyn_qos=%u, timeout=%lu", __entry->pid, __entry->flag, __entry->static_qos, __entry->dyn_qos, __entry->timeout)
);

TRACE_EVENT(qi_futex_inherit,
	TP_PROTO(int ret, struct task_struct *from, struct task_struct *to, unsigned long lock, u32 static_qos, u32 dyn_qos),
	TP_ARGS(ret, from, to, lock, static_qos, dyn_qos),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(pid_t, from)
		__field(pid_t, to)
		__field(unsigned long, lock)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->from = from->pid;
		__entry->to = to->pid;
		__entry->lock = lock;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
	),
	TP_printk("futex inherit: ret=%d from=%d to=%d lock=%lu static_qos=%u dyn_qos=%u", __entry->ret, __entry->from, __entry->to, __entry->lock, __entry->static_qos, __entry->dyn_qos)
);

TRACE_EVENT(qi_futex_uninherit,
	TP_PROTO(int ret, struct task_struct *pid, unsigned long lock, u32 static_qos, u32 dyn_qos),
	TP_ARGS(ret, pid, lock, static_qos, dyn_qos),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(pid_t, pid)
		__field(unsigned long, lock)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->pid = pid->pid;
		__entry->lock = lock;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
	),
	TP_printk("futex uninherit: ret=%d pid=%d static_qos=%u dyn_qos=%u lock=%lu", __entry->ret, __entry->pid, __entry->static_qos, __entry->dyn_qos, __entry->lock)
);

TRACE_EVENT(qi_mutex_inherit,
	TP_PROTO(int ret, struct task_struct *from, struct task_struct *to, unsigned long lock, u32 static_qos, u32 dyn_qos),
	TP_ARGS(ret, from, to, lock, static_qos, dyn_qos),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(pid_t, from)
		__field(pid_t, to)
		__field(unsigned long, lock)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->from = from->pid;
		__entry->to = to->pid;
		__entry->lock = lock;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
	),
	TP_printk("mutex inherit: ret=%d from=%d to=%d lock=%lu, static_qos=%u dyn_qos=%u", __entry->ret, __entry->from, __entry->to, __entry->lock, __entry->static_qos, __entry->dyn_qos)
);

TRACE_EVENT(qi_mutex_jump,
	TP_PROTO(struct task_struct *pid, unsigned long lock),
	TP_ARGS(pid, lock),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(unsigned long, lock)
	),
	TP_fast_assign(
		__entry->pid = pid->pid;
		__entry->lock = lock;
	),
	TP_printk("mutex jump: pid=%d lock=%lu", __entry->pid, __entry->lock)
);

TRACE_EVENT(qi_mutex_uninherit,
	TP_PROTO(int ret, struct task_struct *pid, u32 static_qos, u32 dyn_qos, unsigned long lock),
	TP_ARGS(ret, pid, static_qos, dyn_qos, lock),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(pid_t, pid)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
		__field(unsigned long, lock)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->pid = pid->pid;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
		__entry->lock = lock;
	),
	TP_printk("mutex uninherit: ret=%d pid=%d static_qos=%u dyn_qos=%u lock=%lu", __entry->ret, __entry->pid, __entry->static_qos, __entry->dyn_qos, __entry->lock)
);

TRACE_EVENT(qi_percpu_rwsem_inherit,
	TP_PROTO(int ret, struct task_struct *from, struct task_struct *to, unsigned long lock, u32 static_qos, u32 dyn_qos),
	TP_ARGS(ret, from, to, lock, static_qos, dyn_qos),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(pid_t, from)
		__field(pid_t, to)
		__field(unsigned long, lock)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->from = from->pid;
		__entry->to = to->pid;
		__entry->lock = lock;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
	),
	TP_printk("percpu-rwsem inherit: ret=%d from=%d to=%d lock=%lu static_qos=%u dyn_qos=%u", __entry->ret, __entry->from, __entry->to, __entry->lock, __entry->static_qos, __entry->dyn_qos)
);

TRACE_EVENT(qi_percpu_rwsem_jump,
	TP_PROTO(struct task_struct *pid, unsigned long lock),
	TP_ARGS(pid, lock),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(unsigned long, lock)
	),
	TP_fast_assign(
		__entry->pid = pid->pid;
		__entry->lock = lock;
	),
	TP_printk("percpu-rwsem jump: pid:%d lock=%lu", __entry->pid, __entry->lock)
);

TRACE_EVENT(qi_percpu_rwsem_skip,
	TP_PROTO(struct task_struct *pid, bool is_reader, bool qos_rt_task),
	TP_ARGS(pid, is_reader, qos_rt_task),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(bool, is_reader)
		__field(bool, qos_rt_task)
	),
	TP_fast_assign(
		__entry->pid = pid->pid;
		__entry->is_reader = is_reader;
		__entry->qos_rt_task = qos_rt_task;
	),
	TP_printk("percpu-rwsem skip: pid:%d is_reader=%u qos_rt_task=%u", __entry->pid, __entry->is_reader, __entry->qos_rt_task)
);

TRACE_EVENT(qi_percpu_rwsem_uninherit,
	TP_PROTO(int ret, struct task_struct *pid, u32 static_qos, u32 dyn_qos, unsigned long lock),
	TP_ARGS(ret, pid, static_qos, dyn_qos, lock),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(pid_t, pid)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
		__field(unsigned long, lock)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->pid = pid->pid;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
		__entry->lock = lock;
	),
	TP_printk("percpu-rwsem uninherit: ret=%d pid=%d static_qos=%u dyn_qos=%u lock=%lu", __entry->ret, __entry->pid, __entry->static_qos, __entry->dyn_qos, __entry->lock)
);

TRACE_EVENT(qi_restore_binder_transaction_pid,
	TP_PROTO(void * trans, pid_t bt_pid, pid_t tsk_pid, const char *label),
	TP_ARGS(trans, bt_pid, tsk_pid, label),
	TP_STRUCT__entry(
		__field(void *, trans)
		__field(pid_t, bt_pid)
		__field(pid_t, tsk_pid)
		__array(char, label, 64)
	),
	TP_fast_assign(
		__entry->trans = trans;
		__entry->bt_pid = bt_pid;
		__entry->tsk_pid = tsk_pid;
		__builtin_memcpy(__entry->label, label, 64);
	),
	TP_printk("trans:%p, restore bt pid:%d, tsk pid=%d label=%s", __entry->trans, __entry->bt_pid, __entry->tsk_pid, __entry->label)
);

TRACE_EVENT(qi_rwsem_inherit,
	TP_PROTO(int ret, struct task_struct *from, struct task_struct *to, unsigned long lock, u32 static_qos, u32 dyn_qos),
	TP_ARGS(ret, from, to, lock, static_qos, dyn_qos),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(pid_t, from)
		__field(pid_t, to)
		__field(unsigned long, lock)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->from = from->pid;
		__entry->to = to->pid;
		__entry->lock = lock;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
	),
	TP_printk("rwsem inherit: ret=%d from=%d to=%d lock=%lu static_qos=%u dyn_qos=%u", __entry->ret, __entry->from, __entry->to, __entry->lock, __entry->static_qos, __entry->dyn_qos)
);

TRACE_EVENT(qi_rwsem_jump,
	TP_PROTO(struct task_struct *pid, unsigned long lock),
	TP_ARGS(pid, lock),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(unsigned long, lock)
	),
	TP_fast_assign(
		__entry->pid = pid->pid;
		__entry->lock = lock;
	),
	TP_printk("rwsem jump: pid:%d lock=%lu", __entry->pid, __entry->lock)
);

TRACE_EVENT(qi_rwsem_reader_owned,
	TP_PROTO(bool uninherited, struct task_struct *pid, u32 static_qos, u32 dyn_qos, unsigned long long lock),
	TP_ARGS(uninherited, pid, static_qos, dyn_qos, lock),
	TP_STRUCT__entry(
		__field(bool, uninherited)
		__field(pid_t, pid)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
		__field(unsigned long long, lock)
	),
	TP_fast_assign(
		__entry->uninherited = uninherited;
		__entry->pid = pid->pid;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
		__entry->lock = lock;
	),
	TP_printk("rwsem reader:uninherited=%d cur=%d static_qos=%u dyn_qos=%u lock=%llu", __entry->uninherited, __entry->pid, __entry->static_qos, __entry->dyn_qos, __entry->lock)
);

TRACE_EVENT(qi_rwsem_skip,
	TP_PROTO(struct task_struct *pid, bool is_reader, bool qos_rt_task),
	TP_ARGS(pid, is_reader, qos_rt_task),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(bool, is_reader)
		__field(bool, qos_rt_task)
	),
	TP_fast_assign(
		__entry->pid = pid->pid;
		__entry->is_reader = is_reader;
		__entry->qos_rt_task = qos_rt_task;
	),
	TP_printk("rwsem skip: pid:%d is_reader=%u qos_rt_task=%u", __entry->pid, __entry->is_reader, __entry->qos_rt_task)
);

TRACE_EVENT(qi_rwsem_uninherit,
	TP_PROTO(int ret, struct task_struct *pid, u32 static_qos, u32 dyn_qos, unsigned long lock),
	TP_ARGS(ret, pid, static_qos, dyn_qos, lock),
	TP_STRUCT__entry(
		__field(int, ret)
		__field(pid_t, pid)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
		__field(unsigned long, lock)
	),
	TP_fast_assign(
		__entry->ret = ret;
		__entry->pid = pid->pid;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
		__entry->lock = lock;
	),
	TP_printk("rwsem uninherit: ret=%d pid=%d static_qos=%u dyn_qos=%u lock=%lu", __entry->ret, __entry->pid, __entry->static_qos, __entry->dyn_qos, __entry->lock)
);

TRACE_EVENT(qi_set_binder_transaction_pid,
	TP_PROTO(void * trans, pid_t bt_pid, pid_t tsk_pid, const char *label),
	TP_ARGS(trans, bt_pid, tsk_pid, label),
	TP_STRUCT__entry(
		__field(void *, trans)
		__field(pid_t, bt_pid)
		__field(pid_t, tsk_pid)
		__array(char, label, 64)
	),
	TP_fast_assign(
		__entry->trans = trans;
		__entry->bt_pid = bt_pid;
		__entry->tsk_pid = tsk_pid;
		__builtin_memcpy(__entry->label, label, 64);
	),
	TP_printk("trans:%p, set bt pid:%d, tsk pid=%d label=%s", __entry->trans, __entry->bt_pid, __entry->tsk_pid, __entry->label)
);

TRACE_EVENT(qi_set_qos,
	TP_PROTO(struct task_struct *pid, u32 static_qos, u32 dyn_qos),
	TP_ARGS(pid, static_qos, dyn_qos),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(u32, static_qos)
		__field(u32, dyn_qos)
	),
	TP_fast_assign(
		__entry->pid = pid->pid;
		__entry->static_qos = static_qos;
		__entry->dyn_qos = dyn_qos;
	),
	TP_printk("set qos: pid=%d static_qos=%u dyn_qos=%u", __entry->pid, __entry->static_qos, __entry->dyn_qos)
);

TRACE_EVENT(qi_wake_new_rt_task,
	TP_PROTO(pid_t from_pid, pid_t to_pid, int to_static_qos, int to_dyn_qos),
	TP_ARGS(from_pid, to_pid, to_static_qos, to_dyn_qos),
	TP_STRUCT__entry(
		__field(pid_t, from_pid)
		__field(pid_t, to_pid)
		__field(int, to_static_qos)
		__field(int, to_dyn_qos)
	),
	TP_fast_assign(
		__entry->from_pid = from_pid;
		__entry->to_pid = to_pid;
		__entry->to_static_qos = to_static_qos;
		__entry->to_dyn_qos = to_dyn_qos;
	),
	TP_printk("from_pid:%d, to_pid:%d,  to_static_qos:%d, to_dyn_qos:%d", __entry->from_pid, __entry->to_pid, __entry->to_static_qos, __entry->to_dyn_qos)
);

TRACE_EVENT(qos_mutex_opt_spin,
	TP_PROTO(pid_t pid, bool is_crit, u64 start_time, u64 delta),
	TP_ARGS(pid, is_crit, start_time, delta),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(bool, is_crit)
		__field(u64, start_time)
		__field(u64, delta)
	),
	TP_fast_assign(
		__entry->pid = pid;
		__entry->is_crit = is_crit;
		__entry->start_time = start_time;
		__entry->delta = delta;
	),
	TP_printk("pid=%d, is_crit=%d, start_time=%llu, delta=%llu", __entry->pid, __entry->is_crit, __entry->start_time, __entry->delta)
);

TRACE_EVENT(qos_mutex_wait,
	TP_PROTO(bool start, pid_t pid, u64 cnt),
	TP_ARGS(start, pid, cnt),
	TP_STRUCT__entry(
		__field(bool, start)
		__field(pid_t, pid)
		__field(u64, cnt)
	),
	TP_fast_assign(
		__entry->start = start;
		__entry->pid = pid;
		__entry->cnt = cnt;
	),
	TP_printk("%s: pid=%d, cnt=%llu", __entry->start ? "start" : "finish", __entry->pid, __entry->cnt)
);

TRACE_EVENT(qos_rwsem_opt_rspin,
	TP_PROTO(pid_t pid, u64 start, int state, bool taken),
	TP_ARGS(pid, start, state, taken),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(u64, start)
		__field(int, state)
		__field(bool, taken)
	),
	TP_fast_assign(
		__entry->pid = pid;
		__entry->start = start;
		__entry->state = state;
		__entry->taken = taken;
	),
	TP_printk("pid=%d, start=%llu, state=%x, taken=%d", __entry->pid, __entry->start, __entry->state, __entry->taken)
);

TRACE_EVENT(qos_rwsem_wait,
	TP_PROTO(bool start, pid_t pid, u64 cnt),
	TP_ARGS(start, pid, cnt),
	TP_STRUCT__entry(
		__field(bool, start)
		__field(pid_t, pid)
		__field(u64, cnt)
	),
	TP_fast_assign(
		__entry->start = start;
		__entry->pid = pid;
		__entry->cnt = cnt;
	),
	TP_printk("%s: pid=%d, cnt=0x%llx", __entry->start ? "start" : "finish", __entry->pid, __entry->cnt)
);

#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE qi-trace
#include <trace/define_trace.h>
