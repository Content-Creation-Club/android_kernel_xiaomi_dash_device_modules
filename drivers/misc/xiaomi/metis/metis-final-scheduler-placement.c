// SPDX-License-Identifier: GPL-2.0-only
/* Shared placement algorithms. Scalar carriers keep their explicit native
 * interface; the stock assembly entries use opaque provider-context readers
 * at the original policy stages. Original canonical C entry types remain
 * UNKNOWN. No native fair.c layout or substitute provider is introduced. */
#include <linux/sched.h>
#include <asm/cpufeature.h>
#include <asm/thread_info.h>
#include <sched/sched.h>
#include "metis-final-scheduler.h"
#include "metis-final-owners.h"
#include "metis-stock-context.h"
#include "metis-stock-util.h"

static __always_inline void placement_bound(unsigned int cpu)
{
	if (cpu >= 32) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
}

/* Original 00121cd8 / .text+0x192b0..0x19650 policy. A stock context
 * defers cluster/end reads; scalar native callers already supply them. */
static int tp_choose_policy(struct task_struct *task,
		unsigned long task_util, int cluster, int end_index,
		struct cpumask *allowed, metis_final_native_util_fn native_util,
		const void *stock_context)
{
	u32 bits;
	unsigned int cpu, chosen, row = 0;
	unsigned long best_util, util, best_load, load, capacity;
	if ((!mi_vip_task(task) && !mi_link_vip_task(task)) ||
		!native_util || (stock_context && !get_cpu_util_without) ||
		!metis_lb_cpu_array)
		return -1;
	metis_wakeup_cold_start_skip_ui_cpumask(task, allowed);
	bits = (u32)cpumask_bits(allowed)[0];
	chosen = bits ? __ffs(bits) : 32;
	if (chosen >= nr_cpu_ids)
		return -1;
	cpu = task_cpu(task);
	if (cpumask_test_cpu(cpu, allowed) && available_idle_cpu(cpu))
		return cpu;
	if (stock_context)
		cluster = metis_stock_context_cluster(stock_context);
	best_util = native_util(chosen, task, -1, 0);
	best_load = get_cpu_layered_load(chosen);
	do {
		bits = (u32)cpumask_bits(allowed)[0] &
			(u32)cpumask_bits(metis_lb_cpu_array[cluster][row])[0];
		while (bits) {
			cpu = __ffs(bits);
			bits &= bits - 1;
			if (cpu == ui_run_cpu || cpu == render_run_cpu)
				continue;
			if (available_idle_cpu(cpu))
				return cpu;
			load = get_cpu_layered_load(cpu);
			if (load > best_load)
				continue;
			util = native_util(cpu, task, -1, 0);
			if (load != best_load || util < best_util) {
				best_util = util;
				best_load = load;
				chosen = cpu;
			}
		}
	} while ((s32)row < (stock_context ?
		metis_stock_context_end(stock_context) : end_index) && ++row != 3);
	placement_bound(chosen);
	/* Stock provider capacity_of reads rq+b58; original comparison reads
 	 * rq+b60, the adjacent cpu_capacity_orig, not cpu_capacity. */
	capacity = cpu_rq(chosen)->cpu_capacity_orig;
	if (best_util + task_util >= capacity) {
		if (!mi_sched_choose_cpu_overutiled(task))
			return -1;
		if (metis_tp_debug)
			pr_info("metis-tp: choose cpu overutiled: pid=%d, comm=%s, util=%lu, capacity=%lu",
				task->pid, task->comm, best_util + task_util, capacity);
	}
	return chosen;
}

int metis_final_tp_choose_policy(struct task_struct *task,
		unsigned long task_util, int cluster, int end_index,
		struct cpumask *allowed, metis_final_native_util_fn native_util)
{
	return tp_choose_policy(task, task_util, cluster, end_index, allowed,
			native_util, NULL);
}

/* Original 001194bc / .text+0x10a94..0x10e04 policy. The assembly
 * entry loads excluded before enable callbacks; skipped is loaded here
 * only after allowed-mask construction, and util only on TP fallback. */
static int choose_policy(struct task_struct *task,
		unsigned long task_util, struct cpumask *skipped,
		const struct cpumask *excluded, int cluster, int end_index,
		bool *skip, metis_final_native_util_fn native_util,
		const void *stock_context)
{
	struct cpumask allowed, game_skip;
	u64 word;
	s32 selected;
	if (!metis_sched_enable())
		return -1;
	cpumask_clear(&allowed);
	cpumask_clear(&game_skip);
	word = cpumask_bits(task->cpus_ptr)[0] &
		(u32)(cpumask_bits(cpu_active_mask)[0] &
			~(u32)cpumask_bits(excluded)[0]);
	if (test_tsk_thread_flag(task, TIF_32BIT))
		word &= cpumask_bits(system_32bit_el0_cpumask())[0];
	cpumask_bits(&allowed)[0] = word;
	if (stock_context)
		skipped = metis_stock_context_skipped(stock_context);
	if (render_run_cpu >= 0) {
		cpumask_set_cpu(render_run_cpu, skipped);
		*skip = true;
	}
	if (ui_run_cpu >= 0) {
		cpumask_set_cpu(ui_run_cpu, skipped);
		*skip = true;
	}
	selected = (s32)stask_prefer_cpu;
	/* Register-variable shifts mask CPU indices to six bits in stock. */
	if (coldstart_app_ui_task(task) && (word >> (stask_prefer_cpu & 63) & 1) && selected >= 0)
		return selected;
	if (mi_ui_task(task)) {
		selected = ui_run_cpu;
	} else if (mi_render_task(task)) {
		selected = render_run_cpu;
	} else {
		selected = -1;
	}
	if (selected >= 0 && (word >> ((u32)selected & 63) & 1))
		return selected;
	if (game_migt_skip_cpus_metis_hook) {
		game_migt_skip_cpus_metis_hook(&game_skip);
		cpumask_bits(&allowed)[0] =
			(u32)word & ~(u32)cpumask_bits(&game_skip)[0];
	}
	if (stock_context)
		return metis_stock_tp_choose_context(task, stock_context, &allowed,
				metis_stock_context_util(stock_context));
	return tp_choose_policy(task, task_util, cluster, end_index, &allowed,
			native_util, NULL);
}

int metis_final_choose_policy(struct task_struct *task,
		unsigned long task_util, struct cpumask *skipped,
		const struct cpumask *excluded, int cluster, int end_index,
		bool *skip, metis_final_native_util_fn native_util)
{
	return choose_policy(task, task_util, skipped, excluded, cluster,
			end_index, skip, native_util, NULL);
}

/* These are new local direct-call contracts used by the assembly entries.
 * They declare no guessed original C prototype. Stock-util classification,
 * presence guard and each actual callback read occur in the shared policy. */
int metis_stock_choose_context(struct task_struct *task, const void *context,
		bool *skip, const struct cpumask *excluded)
{
	return choose_policy(task, 0, NULL, excluded, 0, 0, skip,
			metis_final_stock_util_next, context);
}

int metis_stock_tp_choose_context(struct task_struct *task, const void *context,
		struct cpumask *allowed, unsigned long task_util)
{
	return tp_choose_policy(task, task_util, 0, 0, allowed,
			metis_final_stock_util_next, context);
}
