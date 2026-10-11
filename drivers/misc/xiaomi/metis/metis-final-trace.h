/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM metis

#if !defined(_XIAOMI_METIS_FINAL_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _XIAOMI_METIS_FINAL_TRACE_H
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include "metis_common.h"
#include "metis-final-control.h"

/* Remaining 23 actual stock events. Field types/lengths/signedness and
 * print formats come from relocated original .data descriptors, with
 * assignment paths recovered from actual raw-event bodies. Native event
 * ABI/KCFI/BPF/layout/runtime remain UNTESTED; original qualifier/typedef
 * spelling is unknown. The seven existing event definitions stay in their
 * original three headers and retain their existing CREATE_TRACE_POINTS owners.
 * Fixed-width string reads and unterminated arrays retain stock behavior. */

/* Original raw 0011215c; .data fields0x478, print0x5b0. */
TRACE_EVENT(metis_set_biner_alloc_new_buf_vip,
	TP_PROTO(struct task_struct *task, struct task_struct *other, int duration, u32 flags),
	TP_ARGS(task, other, duration, flags),
	TP_STRUCT__entry(
		__field(pid_t, curr_pid)
		__array(char, curr_comm, TASK_COMM_LEN)
		__field(pid_t, t_tsk_pid)
		__array(char, t_tsk_comm, TASK_COMM_LEN)
		__field(unsigned int, msec_jif)
		__field(int, flag)
	),
	TP_fast_assign(
		__entry->curr_pid = task->pid;
		memcpy(__entry->curr_comm, task->comm, TASK_COMM_LEN);
		__entry->t_tsk_pid = other->pid;
		memcpy(__entry->t_tsk_comm, other->comm, TASK_COMM_LEN);
		__entry->msec_jif = duration;
		__entry->flag = flags;
	),
	TP_printk("metis_set_biner_alloc_new_buf_vip : curr_pid=%d curr_comm=%s t_tsk_pid=%d t_tsk_comm=%s boost_ms_jif=%d flag=%x", __entry->curr_pid, __entry->curr_comm, __entry->t_tsk_pid, __entry->t_tsk_comm, __entry->msec_jif, __entry->flag)
);

/* Original raw 00112374; .data fields0x700, print0x7c0. */
TRACE_EVENT(metis_log,
	TP_PROTO(struct task_struct *task, const char *message),
	TP_ARGS(task, message),
	TP_STRUCT__entry(
		__array(char, comm, TASK_COMM_LEN)
		__field(pid_t, pid)
		__array(char, msg, 64)
	),
	TP_fast_assign(
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->pid = task->pid;
		strscpy(__entry->msg, message, 64);
	),
	TP_printk("metis: comm=%s pid=%d msg=%s", __entry->comm, __entry->pid, __entry->msg)
);

/* Original raw 001125d4; .data fields0x880, print0x9b8. */
TRACE_EVENT(lock_spin_dur,
	TP_PROTO(struct task_struct *task, bool taken, u64 elapsed, const char *type, bool vip),
	TP_ARGS(task, taken, elapsed, type, vip),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__array(char, type, TASK_COMM_LEN)
		__field(bool, taken)
		__field(u64, dur)
		__field(bool, vip)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		memcpy(__entry->type, type, TASK_COMM_LEN);
		__entry->taken = taken;
		__entry->dur = elapsed;
		__entry->vip = vip;
	),
	TP_printk("[vip %d]tsk %d %s spin finished with lock taken %d, lock type %s, dur %llu ", __entry->vip, __entry->pid, __entry->comm, __entry->taken, __entry->type, __entry->dur)
);

/* Original raw 00112a14; .data fields0xd00, print0xd98. */
TRACE_EVENT(metis_enqueue_vip_task,
	TP_PROTO(struct task_struct *task),
	TP_ARGS(task),
	TP_STRUCT__entry(
		__field(int, pid)
		__array(char, comm, TASK_COMM_LEN)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
	),
	TP_printk("pid=%d comm=%s", __entry->pid, __entry->comm)
);

/* Original raw 00112bc8; .data fields0xe40, print0xf28. */
TRACE_EVENT(mi_set_rwsem_vip_task,
	TP_PROTO(struct task_struct *task, struct task_struct *owner),
	TP_ARGS(task, owner),
	TP_STRUCT__entry(
		__field(pid_t, cur_pid)
		__field(int, cur_prio)
		__field(pid_t, tsk_pid)
		__field(int, tsk_prio)
	),
	TP_fast_assign(
		__entry->cur_pid = task->pid;
		__entry->cur_prio = task->prio;
		__entry->tsk_pid = owner->pid;
		__entry->tsk_prio = owner->prio;
	),
	TP_printk("%d [%d] set %d [%d] rwsem vip task", __entry->cur_pid, __entry->cur_prio, __entry->tsk_pid, __entry->tsk_prio)
);

/* Original raw 00112d9c; .data fields0x1008, print0x1140. */
TRACE_EVENT(metis_deadline_enqueue_task,
	TP_PROTO(struct mi_task_struct *state, unsigned int cpu, u64 now),
	TP_ARGS(state, cpu, now),
	TP_STRUCT__entry(
		__field(int, pid)
		__field(int, cpu)
		__array(char, comm, TASK_COMM_LEN)
		__field(u64, cur_time)
		__field(u64, sched_deadline)
		__field(u64, remain_period)
	),
	TP_fast_assign(
		__entry->pid = state->task->pid;
		__entry->cpu = cpu;
		memcpy(__entry->comm, state->task->comm, TASK_COMM_LEN);
		__entry->cur_time = now;
		__entry->sched_deadline = MI_FIELD(state, u64, 0x320);
		__entry->remain_period = MI_FIELD(state, u64, 0x328);
	),
	TP_printk("pid=%d comm=%s cpu=%d cur_time=%llu deadline=%llu remain_period=%llu", __entry->pid, __entry->comm, __entry->cpu, __entry->cur_time, __entry->sched_deadline, __entry->remain_period)
);

/* Original raw 00112fa0; .data fields0x1260, print0x1398. */
TRACE_EVENT(metis_deadline_dequeue_task,
	TP_PROTO(struct mi_task_struct *state, unsigned int cpu, u64 now),
	TP_ARGS(state, cpu, now),
	TP_STRUCT__entry(
		__field(int, pid)
		__field(int, cpu)
		__array(char, comm, TASK_COMM_LEN)
		__field(u64, cur_time)
		__field(u64, sched_deadline)
		__field(u64, remain_period)
	),
	TP_fast_assign(
		__entry->pid = state->task->pid;
		__entry->cpu = cpu;
		memcpy(__entry->comm, state->task->comm, TASK_COMM_LEN);
		__entry->cur_time = now;
		__entry->sched_deadline = MI_FIELD(state, u64, 0x320);
		__entry->remain_period = MI_FIELD(state, u64, 0x328);
	),
	TP_printk("pid=%d comm=%s cpu=%d cur_time=%llu deadline=%llu remain_period=%llu", __entry->pid, __entry->comm, __entry->cpu, __entry->cur_time, __entry->sched_deadline, __entry->remain_period)
);

/* Original raw 001131a4; .data fields0x14b8, print0x15f0. */
TRACE_EVENT(metis_deadline_pick_task,
	TP_PROTO(struct mi_task_struct *state, unsigned int cpu, u64 now),
	TP_ARGS(state, cpu, now),
	TP_STRUCT__entry(
		__field(int, pid)
		__field(int, cpu)
		__array(char, comm, TASK_COMM_LEN)
		__field(u64, cur_time)
		__field(u64, sched_deadline)
		__field(u64, remain_period)
	),
	TP_fast_assign(
		__entry->pid = state->task->pid;
		__entry->cpu = cpu;
		memcpy(__entry->comm, state->task->comm, TASK_COMM_LEN);
		__entry->cur_time = now;
		__entry->sched_deadline = MI_FIELD(state, u64, 0x320);
		__entry->remain_period = MI_FIELD(state, u64, 0x328);
	),
	TP_printk("pid=%d comm=%s cpu=%d cur_time=%llu deadline=%llu remain_period=%llu", __entry->pid, __entry->comm, __entry->cpu, __entry->cur_time, __entry->sched_deadline, __entry->remain_period)
);

/* Original raw 00113584; .data fields0x18c8, print0x1b40. */
TRACE_EVENT(metis_vip_task_flag,
	TP_PROTO(struct mi_task_struct *state, unsigned long now),
	TP_ARGS(state, now),
	TP_STRUCT__entry(
		__field(int, ppid)
		__field(int, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(int, flag)
		__field(int, config_index)
		__field(int, p_index)
		__field(bool, static_vip)
		__field(bool, topapp_vip)
		__field(bool, main_thread_depend)
		__field(bool, deadline)
		__field(bool, deadline_depend_main)
		__field(unsigned long, jiffies)
		__field(unsigned long, boost_end_jif)
		__field(unsigned long, touch_jif)
	),
	TP_fast_assign(
		/* Stock field ppid holds TGID, not native parent PID. */
		__entry->ppid = state->task->tgid;
		__entry->pid = state->task->pid;
		memcpy(__entry->comm, state->task->comm, TASK_COMM_LEN);
		__entry->flag = MI_FIELD(state, u32, 0xc0);
		__entry->config_index = MI_FIELD(state, u32, 0x2f8);
		__entry->p_index = get_process_config_index(state->task);
		__entry->static_vip = is_static_vip(state);
		__entry->topapp_vip = is_top_app_vip(state);
		__entry->main_thread_depend = is_main_thread_depend(state);
		__entry->deadline = is_deadline_task(state);
		__entry->deadline_depend_main = is_deadline_depend_main_task(state);
		__entry->jiffies = now;
		__entry->boost_end_jif = MI_FIELD(state, unsigned long, 0x290);
		__entry->touch_jif = MI_FIELD(state, unsigned long, 0x278);
	),
	TP_printk("pid=%d comm=%s flag=%d ppid=%d config_index=%d p_index=%d static_vip=%d topapp_vip=%d main_thread_depend=%d deadline=%d deadline_depend_main=%d jiffies=%lu boost_end_jif=%lu touch_jif=%lu", __entry->pid, __entry->comm, __entry->flag, __entry->ppid, __entry->config_index, __entry->p_index, __entry->static_vip, __entry->topapp_vip, __entry->main_thread_depend, __entry->deadline, __entry->deadline_depend_main, jiffies, __entry->boost_end_jif, __entry->touch_jif)
);

/* Original raw 00113878; .data fields0x1d60, print0x1e70. */
TRACE_EVENT(metis_task_type,
	TP_PROTO(struct task_struct *task, int link_vip, int art_mutex_vip, int vip),
	TP_ARGS(task, link_vip, art_mutex_vip, vip),
	TP_STRUCT__entry(
		__field(int, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(int, link_vip)
		__field(int, art_mutex_vip)
		__field(int, mi_vip)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->link_vip = link_vip;
		__entry->art_mutex_vip = art_mutex_vip;
		__entry->mi_vip = vip;
	),
	TP_printk("pid=%d comm=%s link_vip=%d art_mutex_vip=%d mi_vip=%d", __entry->pid, __entry->comm, __entry->link_vip, __entry->art_mutex_vip, __entry->mi_vip)
);

/* Original raw 00113c60; .data fields0x2130, print0x21c8. */
TRACE_EVENT(metis_iowait_status_update,
	TP_PROTO(struct task_struct *task, u32 flags),
	TP_ARGS(task, flags),
	TP_STRUCT__entry(
		__field(int, pid)
		__field(int, flag)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		__entry->flag = flags;
	),
	TP_printk("pid=%d flag=%d", __entry->pid, __entry->flag)
);

/* Original raw 00113e04; .data fields0x2270, print0x2330. */
TRACE_EVENT(metis_verity_work_start,
	TP_PROTO(struct task_struct *task, unsigned long elapsed, unsigned long window),
	TP_ARGS(task, elapsed, window),
	TP_STRUCT__entry(
		__field(int, pid)
		__field(unsigned long, diff)
		__field(unsigned long, io_jif)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		__entry->diff = elapsed;
		__entry->io_jif = window;
	),
	TP_printk("pid=%d diff=%lu io_jif=%lu", __entry->pid, __entry->diff, __entry->io_jif)
);

/* Original raw 00113fc0; .data fields0x23f0, print0x24b0. */
TRACE_EVENT(mi_wakeup_link,
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
	TP_printk("comm = %s pid = %d type = %s", __entry->comm, __entry->pid, __entry->type)
);

/* Original raw 00114228; .data fields0x2570, print0x2630. */
TRACE_EVENT(mi_pick_next_vip_task,
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
	TP_printk("comm = %s pid = %d type = %s", __entry->comm, __entry->pid, __entry->type)
);

/* Original raw 001146f8; .data fields0x2870, print0x2980. */
TRACE_EVENT(mi_set_binder_vip_task,
	TP_PROTO(struct task_struct *task, struct task_struct *owner, u64 mask),
	TP_ARGS(task, owner, mask),
	TP_STRUCT__entry(
		__field(pid_t, cur_pid)
		__field(pid_t, tsk_pid)
		__field(u64, mask)
		__field(int, cur_prio)
		__field(int, tsk_prio)
	),
	TP_fast_assign(
		__entry->cur_pid = task->pid;
		__entry->tsk_pid = owner->pid;
		__entry->mask = mask;
		__entry->cur_prio = task->prio;
		__entry->tsk_prio = owner->prio;
	),
	TP_printk("%d [%d] set %d [%d] binder vip task, mask %llu", __entry->cur_pid, __entry->cur_prio, __entry->tsk_pid, __entry->tsk_prio, __entry->mask)
);

/* Original raw 001148e4; .data fields0x2a78, print0x2b88. */
TRACE_EVENT(mi_clean_binder_vip_task,
	TP_PROTO(struct task_struct *task, struct task_struct *owner, u64 mask),
	TP_ARGS(task, owner, mask),
	TP_STRUCT__entry(
		__field(pid_t, cur_pid)
		__field(pid_t, tsk_pid)
		__field(u64, mask)
		__field(int, cur_prio)
		__field(int, tsk_prio)
	),
	TP_fast_assign(
		__entry->cur_pid = task->pid;
		__entry->tsk_pid = owner->pid;
		__entry->mask = mask;
		__entry->cur_prio = task->prio;
		__entry->tsk_prio = owner->prio;
	),
	TP_printk("%d [%d] clean %d [%d] binder vip task, mask %llu", __entry->cur_pid, __entry->cur_prio, __entry->tsk_pid, __entry->tsk_prio, __entry->mask)
);

/* Original raw 00114ad0; .data fields0x2c80, print0x2d18. */
TRACE_EVENT(mi_get_futex_owner_pid,
	TP_PROTO(int pid, int owner_pid),
	TP_ARGS(pid, owner_pid),
	TP_STRUCT__entry(
		__field(pid_t, cur)
		__field(pid_t, owner)
	),
	TP_fast_assign(
		__entry->cur = pid;
		__entry->owner = owner_pid;
	),
	TP_printk("curr %d get futex_owner %d from uaddr2", __entry->cur, __entry->owner)
);

/* Original raw 00114c6c; .data fields0x2dd8, print0x2ee8. */
TRACE_EVENT(mi_set_futex_vip_task,
	TP_PROTO(struct task_struct *task, struct task_struct *owner, u64 mask),
	TP_ARGS(task, owner, mask),
	TP_STRUCT__entry(
		__field(pid_t, cur_pid)
		__field(pid_t, tsk_pid)
		__field(u64, mask)
		__field(int, cur_prio)
		__field(int, tsk_prio)
	),
	TP_fast_assign(
		__entry->cur_pid = task->pid;
		__entry->tsk_pid = owner->pid;
		__entry->mask = mask;
		__entry->cur_prio = task->prio;
		__entry->tsk_prio = owner->prio;
	),
	TP_printk("%d [%d] set %d [%d] futex vip task, mask %llu", __entry->cur_pid, __entry->cur_prio, __entry->tsk_pid, __entry->tsk_prio, __entry->mask)
);

/* Original raw 00114e58; .data fields0x2fe0, print0x30f0. */
TRACE_EVENT(mi_clean_futex_vip_task,
	TP_PROTO(struct task_struct *task, struct task_struct *owner, u64 mask),
	TP_ARGS(task, owner, mask),
	TP_STRUCT__entry(
		__field(pid_t, cur_pid)
		__field(pid_t, tsk_pid)
		__field(u64, mask)
		__field(int, cur_prio)
		__field(int, tsk_prio)
	),
	TP_fast_assign(
		__entry->cur_pid = task->pid;
		__entry->tsk_pid = owner->pid;
		__entry->mask = mask;
		__entry->cur_prio = task->prio;
		__entry->tsk_prio = owner->prio;
	),
	TP_printk("%d [%d] clean %d [%d] futex vip task, mask %llu", __entry->cur_pid, __entry->cur_prio, __entry->tsk_pid, __entry->tsk_prio, __entry->mask)
);

/* Original raw 00115044; .data fields0x31e8, print0x32d0. */
TRACE_EVENT(mi_set_mutex_vip_task,
	TP_PROTO(struct task_struct *task, struct task_struct *owner),
	TP_ARGS(task, owner),
	TP_STRUCT__entry(
		__field(pid_t, cur_pid)
		__field(int, cur_prio)
		__field(pid_t, tsk_pid)
		__field(int, tsk_prio)
	),
	TP_fast_assign(
		__entry->cur_pid = task->pid;
		__entry->cur_prio = task->prio;
		__entry->tsk_pid = owner->pid;
		__entry->tsk_prio = owner->prio;
	),
	TP_printk("%d [%d] set %d [%d] mutex vip task", __entry->cur_pid, __entry->cur_prio, __entry->tsk_pid, __entry->tsk_prio)
);

/* Original raw 00115218; .data fields0x33b0, print0x3510. */
TRACE_EVENT(mi_lock_info,
	TP_PROTO(struct task_struct *task, bool vip, const char *type, int owner_pid, int owner_prio, bool owner_vip),
	TP_ARGS(task, vip, type, owner_pid, owner_prio, owner_vip),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(bool, curr_is_vip)
		__array(char, block_type, 32)
		__field(int, owner_pid)
		__field(int, owner_prio)
		__field(bool, owner_is_vip)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->curr_is_vip = vip;
		memcpy(__entry->block_type, type, 32);
		__entry->owner_pid = owner_pid;
		__entry->owner_prio = owner_prio;
		__entry->owner_is_vip = owner_vip;
	),
	TP_printk("current tsk:%s[%d][%d] blocked by %s. lock owner pid:%d, prio:%d, is vip:%d", __entry->comm, __entry->pid, __entry->curr_is_vip, __entry->block_type, __entry->owner_pid, __entry->owner_prio, __entry->owner_is_vip)
);

/* Original raw 00115478; .data fields0x3650, print0x3738. */
TRACE_EVENT(mi_lock_hold_info,
	TP_PROTO(struct task_struct *task, const char *type, unsigned long duration),
	TP_ARGS(task, type, duration),
	TP_STRUCT__entry(
		__field(int, pid)
		__array(char, comm, TASK_COMM_LEN)
		__array(char, lock_type, 32)
		__field(unsigned long, hold_time)
	),
	TP_fast_assign(
		__entry->pid = task->pid;
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		memcpy(__entry->lock_type, type, 32);
		__entry->hold_time = duration;
	),
	TP_printk("pid=%d comm=%s block_type=%s, hold_time=%lu", __entry->pid, __entry->comm, __entry->lock_type, __entry->hold_time)
);

/* Original raw 00115b80; .data fields0x3ba8, print0x3cb8. */
TRACE_EVENT(mi_restore_lock_vip_task,
	TP_PROTO(struct task_struct *task, struct task_struct *owner, const char *type),
	TP_ARGS(task, owner, type),
	TP_STRUCT__entry(
		__field(pid_t, cur_pid)
		__field(int, cur_prio)
		__field(pid_t, tsk_pid)
		__field(int, tsk_prio)
		__array(char, type, 64)
	),
	TP_fast_assign(
		__entry->cur_pid = task->pid;
		__entry->cur_prio = task->prio;
		__entry->tsk_pid = owner->pid;
		__entry->tsk_prio = owner->prio;
		memcpy(__entry->type, type, 64);
	),
	TP_printk("%d [%d] restore %d [%d] lock vip task, type %s", __entry->cur_pid, __entry->cur_prio, __entry->tsk_pid, __entry->tsk_prio, __entry->type)
);

#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE metis-final-trace
#include <trace/define_trace.h>

