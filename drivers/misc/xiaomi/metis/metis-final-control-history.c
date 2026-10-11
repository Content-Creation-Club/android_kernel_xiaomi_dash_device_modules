// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/panic_notifier.h>
#include <linux/sched.h>
#include <linux/string.h>
#include "kernel/sched/sched.h"
#include "metis-final-control.h"

/* Only Xiaomi's private METIS storage has retained byte offsets here. */
static struct metis_rq *history_rq(struct rq *rq)
{
	u32 cpu = cpu_of(rq);

	if (cpu >= 32) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	return per_cpu_ptr(&metis_runqueues, cpu);
}

static void save_top_pid(struct metis_rq *state, u32 offset,
			 struct task_struct *task)
{
	MI_FIELD(state, s32, offset) = task->pid;
	memcpy((u8 *)state + offset + 4, task->comm, 16);
}

static void update_previous_top(struct task_struct *task, struct rq *rq,
				u64 runtime, bool reject_zero)
{
	struct metis_rq *state = history_rq(rq);
	struct mi_task_struct *mi = get_mi_task_struct(task);

	if ((reject_zero && !runtime) || (task->flags & PF_IDLE) || !mi) return;
	if (MI_FIELD(state, u64, 0x28) <= runtime) {
		MI_FIELD(state, u64, 0x28) = runtime;
		save_top_pid(state, 0x48, task);
	}
	MI_FIELD(mi, u64, 0x288) = 0;
}

/* metis_update_top_task_info 0011d890, .text+0x14e68..0x151fc.
 * Direct mi_schedule_hook supplies (task, native rq, int, u64, u64);
 * original typeword14685f61, full 32-bit W2 is narrowed with &1. */
void metis_update_top_task_info(struct task_struct *task, struct rq *rq,
			       int running, u64 wallclock, u64 previous)
{
	struct metis_rq *state, *again;
	struct mi_task_struct *mi;
	u64 start, last, elapsed, signed_window, window, overlap, runtime;
	s32 periods;
	u32 configured;

	if (!sched_doctor_enable || (rq->curr->flags & PF_IDLE)) return;
	if (!top_task_monitor_window)
		top_task_monitor_window = (u64)((s64)normal_running_too_long_thresh * 1000000000LL) >> 1;
	state = history_rq(rq);
	start = MI_FIELD(state, u64, 0x38);
	last = MI_FIELD(state, u64, 0x40);
	elapsed = wallclock - start;
	if (wallclock < last || (s64)elapsed < 0)
		printk_deferred("Metis-BUG CPU%d; wclock=%llu(0x%llx) l_clock=%llu(0x%llx), w_start=%llu(0x%llx)",
			cpu_of(rq), wallclock, wallclock, last, last, start, start);
	configured = top_task_monitor_window;
	signed_window = (u64)(s64)(s32)configured;
	MI_FIELD(state, u64, 0x40) = wallclock;
	if ((s64)elapsed >= (s64)signed_window) {
		periods = signed_window ? (s32)(elapsed / signed_window) : 0;
		MI_FIELD(state, u64, 0x38) += (s64)periods * (s32)configured;
		again = history_rq(rq);
		if (periods < 2) {
			memcpy((u8 *)again + 0x48, (u8 *)again + 0x70, 0x28);
			MI_FIELD(again, u32, 0x70) = 0;
			MI_FIELD(again, u64, 0x28) = MI_FIELD(again, u64, 0x30);
			MI_FIELD(again, u64, 0x30) = 0;
			top_task_monitor_window = (u64)((s64)normal_running_too_long_thresh * 1000000000LL) >> 1;
		} else {
			MI_FIELD(again, u32, 0x48) = 0;
			MI_FIELD(again, u32, 0x70) = 0;
			MI_FIELD(again, u64, 0x28) = 0;
			MI_FIELD(again, u64, 0x30) = 0;
		}
	}
	state = history_rq(rq);
	mi = get_mi_task_struct(task);
	if (!mi) return;
	start = MI_FIELD(state, u64, 0x38);
	window = top_task_monitor_window ? top_task_monitor_window : 1000000000U;
	if (!(running & 1)) {
		if (start <= previous) return;
		/* Original reads only W0 of the eight-byte runtime on this branch. */
		update_previous_top(task, rq, MI_FIELD(mi, u32, 0x288), true);
		return;
	}
	overlap = start - previous;
	if (start > previous && overlap) {
		periods = window ? (s32)(overlap / window) : 0;
		if (!periods) {
			runtime = MI_FIELD(mi, u64, 0x288) + overlap;
			if ((u64)(s64)(s32)top_task_monitor_window < runtime)
				runtime = (s64)(s32)top_task_monitor_window;
			MI_FIELD(mi, u64, 0x288) = runtime;
			update_previous_top(task, rq, (u32)runtime, true);
		} else {
			update_previous_top(task, rq, window, false);
		}
		previous = start;
	}
	state = history_rq(rq);
	mi = get_mi_task_struct(task);
	if (!mi) return;
	runtime = MI_FIELD(mi, u64, 0x288) + wallclock - previous;
	signed_window = (u64)(s64)(s32)top_task_monitor_window;
	if (runtime > signed_window) runtime = signed_window;
	MI_FIELD(mi, u64, 0x288) = runtime;
	if (MI_FIELD(state, u64, 0x30) < runtime) {
		MI_FIELD(state, u64, 0x30) = runtime;
		save_top_pid(state, 0x70, task);
	}
}

/* history_task_info_init 0011e69c, .text+0x15c74..0x15e18. */
void history_task_info_init(void)
{
	memset(his_nor_task, 0, sizeof(his_nor_task));
	memset(his_rt_task, 0, sizeof(his_rt_task));
	memset(his_max_task, 0, sizeof(his_max_task));
	memset(his_lru_task, 0, sizeof(his_lru_task));
}

/* metis_doublecyc_init 0011e844/.text+0x15e1c; original inlines the
 * same history clears, ignores proc/notifier registration failures. */
int metis_doublecyc_init(void)
{
	history_task_info_init();
	runtime_info_entry = proc_create("show_mi_runtime_info", 0664, NULL,
					 &runtime_info_debug_proc_ops);
	atomic_notifier_chain_register(&panic_notifier_list, &runtime_info_notifier);
	printk("\0016metis-doublecyc: Doublecyc: %s: inited!\n", "metis_doublecyc_init");
	return 0;
}

/* metis_doublecyc_exit 0011ea50/.text+0x16028..0x16084. */
void metis_doublecyc_exit(void)
{
	if (runtime_info_entry) proc_remove(runtime_info_entry);
	runtime_info_entry = NULL;
	atomic_notifier_chain_unregister(&panic_notifier_list, &runtime_info_notifier);
	printk("\0016metis-doublecyc: Doublecyc: %s: exit!\n", "metis_doublecyc_exit");
}

/* runtime_info_debug_open 0011eab0/.text+0x16088; i_private is the
 * actual current native inode field, confirmed stock load before single_open. */
int runtime_info_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, runtime_info_debug_show, inode->i_private);
}

static void history_bounds(u32 index, u32 limit)
{
	if (index >= limit) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
}

/* do_print_record_abnorm_task_info 0011f008, .text+0x165e0..0x16808. */
void do_print_record_abnorm_task_info(struct seq_file *seq, u32 begin,
				    u32 end, int kind)
{
	u32 i = begin;

	if ((s32)begin >= (s32)end) return;
	do {
		if (kind == 2) {
			history_bounds(i, 10);
			if (his_nor_task[i].pid) {
				seq_printf(seq, "pid:%-6d", his_nor_task[i].pid);
				seq_printf(seq, "time(s):%-4ld", (long)his_nor_task[i].running_seconds);
				seq_printf(seq, "top_task_pid:%-6d", his_max_task[i].pid);
				seq_printf(seq, "top_task_runtime(ns):%-12lu", (unsigned long)his_max_task[i].running_seconds);
				seq_printf(seq, " happen_time(ns):%-12llu\n", his_max_task[i].sampled_at);
			}
		} else if (kind == 1) {
			history_bounds(i, 10);
			if (his_rt_task[i].pid) {
				seq_printf(seq, "pid:%-6d", his_rt_task[i].pid);
				seq_printf(seq, "time(s):%-4ld", (long)his_rt_task[i].running_seconds);
				seq_printf(seq, "happen_time(ns):%-12lld\n", (s64)his_rt_task[i].sampled_at);
			}
		} else if (kind == 0) {
			history_bounds(i, 12);
			seq_printf(seq, "pid:%-6d", his_lru_task[i].pid);
			seq_printf(seq, "comm:%15s", his_lru_task[i].comm);
			seq_printf(seq, "  runtime(ns):%-12llu\n", his_lru_task[i].running_time);
		}
	} while (++i != end);
}

/* Extract stock ring traversal without modulo normalization. */
static void show_history_ring(struct seq_file *seq, struct metis_rq *state,
			      u32 offset, int kind)
{
	u32 first = MI_FIELD(state, u32, offset);

	if ((s32)first < 10) {
		do_print_record_abnorm_task_info(seq, first, 10, kind);
		/* Original reloads the producer index after printing the tail. */
		first = MI_FIELD(state, u32, offset);
		if ((s32)first <= 0) return;
	}
	do_print_record_abnorm_task_info(seq, 0, first, kind);
}

/* runtime_info_debug_show 0011eae0, .text+0x160b8..0x165e0;
 * native seq callback has unused void* (CFId7b572f7). */
int runtime_info_debug_show(struct seq_file *seq, void *unused)
{
	static const char *const names[4] = { "RT", "VIP", "IDLE", "NORM" };
	struct metis_rq *state;
	u32 cpu, type;

	for_each_possible_cpu(cpu) {
		if (cpu >= 32) break;
		state = per_cpu_ptr(&metis_runqueues, cpu);
		seq_printf(seq, "\ncpu_index: %d\n", cpu);
		for (type = 0; type < 4; type++)
			seq_printf(seq, "%-4s: %-10lu number_of_times:%6d\n", names[type],
				(unsigned long)MI_FIELD(state, u64, 0xb8 + type * 8),
				MI_FIELD(state, s32, 0x98 + type * 4));
		seq_printf(seq, "\n");
		seq_printf(seq, "\n-----the longest running tasks in the 3s of the current time-----\n");
		seq_printf(seq, "top_task_pid:%-6d", MI_FIELD(state, s32, 0x48));
		seq_printf(seq, "top_task_runtime:%-12llu", MI_FIELD(state, u64, 0x28));
		seq_printf(seq, "\n");
		seq_printf(seq, "\n");
		seq_printf(seq, "-----rq recently scheduled tasks-----\n");
		show_history_ring(seq, state, 0x14, 0);
		seq_printf(seq, "-----abnorm rt_tasks-----\n");
		show_history_ring(seq, state, 0x20, 1);
		seq_printf(seq, "-----abnorml norm_task-----\n");
		do_print_record_abnorm_task_info(seq, MI_FIELD(state, u32, 0x1c), 10, 2);
		do_print_record_abnorm_task_info(seq, 0, MI_FIELD(state, u32, 0x1c), 2);
		seq_printf(seq, "\n1~2s :%2d\n", MI_FIELD(state, s32, 0xac));
		seq_printf(seq, "2~3s :%2d\n", MI_FIELD(state, s32, 0xb0));
		seq_printf(seq, " >3s :%2d\n", MI_FIELD(state, s32, 0xb4));
		seq_printf(seq, "\n-----tasks running continuously for more than 3s-----\n");
		seq_printf(seq, "rt_task_pid:%-6d", his_lru_task[10].pid);
		seq_printf(seq, "  runlongtime(ns):%-12llu", his_lru_task[10].running_time);
		seq_printf(seq, "  happen_time(ns):%-12llu\n", his_lru_task[10].sampled_at);
		seq_printf(seq, "other_task_pid:%-6d", his_lru_task[11].pid);
		seq_printf(seq, "  runlongtime(ns):%-12llu", his_lru_task[11].running_time);
		seq_printf(seq, "  happen_time(ns):%-12llu", his_lru_task[11].sampled_at);
		seq_printf(seq, "\n");
	}
	seq_printf(seq, "\n");
	return 0;
}

static void print_rt_ring(struct metis_rq *state)
{
	u32 i, first = MI_FIELD(state, u32, 0x20);

	if ((s32)first < 10) {
		i = first;
		do {
			history_bounds(i, 10);
			if (his_rt_task[i].pid)
				printk("\0016metis-doublecyc: abnorm_rt_tasks:pid:%-6d happen_time(ns):%-12llu\n",
					his_rt_task[i].pid, his_rt_task[i].sampled_at);
		} while (++i != 10);
		first = MI_FIELD(state, u32, 0x20);
		if ((s32)first <= 0) return;
	}
	for (i = 0; i != first; i++) {
		history_bounds(i, 10);
		if (his_rt_task[i].pid)
			printk("\0016metis-doublecyc: abnorm_rt_tasks:pid:%-6d happen_time(ns):%-12llu\n",
				his_rt_task[i].pid, his_rt_task[i].sampled_at);
	}
}

static void print_normal_range(u32 first, u32 end)
{
	u32 i;

	for (i = first; i != end; i++) {
		history_bounds(i, 10);
		if (his_nor_task[i].pid)
			printk("\0016metis-doublecyc: abnorm_norm_tasks:pid:%-6d time(s):%-4ldtop_task_pid:%-8d happen_time(ns):%-12llu\n",
				his_nor_task[i].pid, (long)his_nor_task[i].running_seconds,
				his_max_task[i].pid, his_max_task[i].sampled_at);
	}
}

/* kmsg_runtime_info_debug_show 0011f234, .text+0x1680c..0x16b50;
 * .data+0x3e98 points at it as notifier_call, original CFI2a4cec24.
 * Native notifier.h supplies all three formals; Ghidra lost all of them. */
int kmsg_runtime_info_debug_show(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	struct metis_rq *state;
	u32 cpu, first;

	for_each_possible_cpu(cpu) {
		if (cpu >= 32) break;
		state = per_cpu_ptr(&metis_runqueues, cpu);
		if (!MI_FIELD(state, u32, 0x98) && !MI_FIELD(state, u32, 0x9c) &&
		    !MI_FIELD(state, u32, 0xa0) && !MI_FIELD(state, u32, 0xa4)) continue;
		printk("\0016metis-doublecyc: cpu: %d\n", cpu);
		print_rt_ring(state);
		first = MI_FIELD(state, u32, 0x1c);
		if ((s32)first < 10) {
			print_normal_range(first, 10);
			first = MI_FIELD(state, u32, 0x1c);
		}
		if ((s32)first > 0) print_normal_range(0, first);
		if (his_lru_task[10].pid)
			printk("\0016metis-doublecyc: run_longtime_rt_tasks:pid:%-6d runtime(ns):%-12llu happen_time(ns):%-12llu\n",
				his_lru_task[10].pid, his_lru_task[10].running_time, his_lru_task[10].sampled_at);
	}
	return 0;
}
