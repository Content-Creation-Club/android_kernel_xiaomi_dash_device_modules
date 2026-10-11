// SPDX-License-Identifier: GPL-2.0-only
/* Stock METIS .text+0x17e60..0x19720. Native rq/task/locking members;
 * MI_FIELD offsets refer only to genuine Xiaomi private driver storage. */
#include <linux/arch_topology.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <sched/sched.h>
#include <trace/hooks/sched.h>
#include "metis-final-scheduler.h"
#include "metis-lowlt-trace.h"

static __always_inline void load_cpu_bound(unsigned int cpu)
{
	if (cpu >= 32) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
}

static struct mi_rq *load_rq(unsigned int cpu)
{
	load_cpu_bound(cpu);
	return per_cpu_ptr(&mi_runqueues, cpu);
}

static void layered_debug(unsigned int cpu, u64 layered, struct mi_rq *rq)
{
	u64 raw = MI_FIELD(rq, u64, 0x1a8);
	if (metis_tp_debug)
		pr_info("metis-tp: layered_load: cpu%d, layered = %llu, raw = %llu, err = %d",
			cpu, layered, raw, raw < layered);
}

/* 00121774 / .text+0x18d4c..0x18de4. */
unsigned long get_cpu_layered_load(unsigned int cpu)
{
	struct mi_rq *rq;
	u64 load;
	if (!mi_viptask_balance)
		return 0;
	rq = load_rq(cpu);
	load = MI_FIELD(rq, u64, 0x178) + MI_FIELD(rq, u64, 0x188);
	layered_debug(cpu, load, rq);
	return load;
}

/* 0012180c / .text+0x18de4..0x18e74. */
unsigned long get_cpu_vip_tasks_load(unsigned int cpu)
{
	struct mi_rq *rq;
	u64 load;
	if (!mi_viptask_balance)
		return 0;
	rq = load_rq(cpu);
	load = MI_FIELD(rq, u64, 0x188);
	layered_debug(cpu, load, rq);
	return load;
}

/* 0012189c / .text+0x18e74..0x18f14: despite its name, VIP-only load.
 * Disabled path leaves the caller's output untouched; conversion is 32-bit. */
bool metis_tp_check_cpu_layered_load(unsigned int cpu, int *load)
{
	struct mi_rq *rq;
	u64 value;
	if (!mi_viptask_balance)
		return false;
	rq = load_rq(cpu);
	value = MI_FIELD(rq, u64, 0x188);
	layered_debug(cpu, value, rq);
	*load = (int)value;
	return *load != 0;
}

/* 0012170c / .text+0x18ce4..0x18d3c. */
bool metis_tp_is_vip_task(struct task_struct *task)
{
	return mi_viptask_balance && (mi_vip_task(task) || mi_link_vip_task(task));
}

/* 00121764 / .text+0x18d3c..0x18d4c. */
int metis_tp_mi_viptask_balance(void)
{
	return mi_viptask_balance;
}

/* 0012193c / .text+0x18f14..0x191f8. Seven actual provider arguments:
 * scheduler.ko flt enqueue/dequeue and update_history call sites. */
void metis_tp_fixup_cumulative_runnable_avg_hook(struct rq *rq,
		struct task_struct *task, long delta, long previous_delta,
		int operation, unsigned long total, unsigned long previous_total)
{
	struct mi_rq *state;
	struct mi_task_struct *task_state;
	u64 *load, *previous_load, value;
	unsigned int cpu;
	if (!mi_viptask_balance)
		return;
	cpu = cpu_of(rq);
	state = load_rq(cpu);
	MI_FIELD(state, u64, 0x1a8) = total;
	MI_FIELD(state, u64, 0x1b0) = previous_total;
	if (operation == 1) {
		if (task->policy - 1U < 2) {
			task_state = get_mi_task_struct(task);
			if (task_state)
				MI_FIELD(task_state, u32, 0x2e4) = 1;
		} else if (mi_vip_task(task) || mi_link_vip_task(task)) {
			task_state = get_mi_task_struct(task);
			if (task_state)
				MI_FIELD(task_state, u32, 0x2e4) = 2;
		}
	}
	task_state = get_mi_task_struct(task);
	if (!task_state)
		return;
	if (MI_FIELD(task_state, u32, 0x2e4) == 2) {
		load = &MI_FIELD(state, u64, 0x188);
		previous_load = &MI_FIELD(state, u64, 0x190);
	} else if (MI_FIELD(task_state, u32, 0x2e4) == 1) {
		load = &MI_FIELD(state, u64, 0x178);
		previous_load = &MI_FIELD(state, u64, 0x180);
	} else {
		return;
	}
	value = *load + (u64)delta;
	*load = (s64)value < 0 ? 0 : value;
	value = *previous_load + (u64)previous_delta;
	*previous_load = (s64)value < 0 ? 0 : value;
	if (operation == -1) {
		task_state = get_mi_task_struct(task);
		if (task_state)
			MI_FIELD(task_state, u32, 0x2e4) = 0;
		if (metis_tp_debug)
			pr_info("metis-tp: layered_load: cpu%d, _dsd = %lld, cras = %llu, _pdsd = %lld, pdss = %llu",
				cpu, (s64)delta, *load, (s64)previous_delta, *previous_load);
	}
	value = 0;
	if (mi_viptask_balance) {
		state = load_rq(cpu);
		value = MI_FIELD(state, u64, 0x178) + MI_FIELD(state, u64, 0x188);
		layered_debug(cpu, value, state);
	}
	/* Raw instructions .text+0x190d0..0x191e4 retain both trace gates,
 	 * omitted by Ghidra's textual decompilation. Actual schema is shared. */
	trace_tracing_mark_write('C', 0, "Layered Load", cpu, (int)value);
	trace_tracing_mark_write('C', 0, "Raw Load", cpu,
		(int)MI_FIELD(load_rq(cpu), u64, 0x1a8));
}

/* 00121c20 / .text+0x191f8..0x192b0. Integer result contract remains
 * inferred from actual machine result/caller; native KCFI is untested. */
int metis_tp_walt_find_best_target_hook(unsigned int cpu, unsigned long *load)
{
	unsigned long layered;
	if (!mi_viptask_balance)
		return 0;
	layered = get_cpu_layered_load(cpu);
	if (*load < layered)
		return 1;
	*load = layered;
	return 0;
}

/* 00122078 / .text+0x19650..0x196fc. */
int metis_tp_init(void)
{
	unsigned int cpu;
	struct mi_rq *rq;
	for_each_possible_cpu(cpu) {
		if (cpu >= 32)
			break;
		rq = load_rq(cpu);
		MI_FIELD(rq, u64, 0x170) = 0;
		MI_FIELD(rq, u64, 0x178) = 0;
		MI_FIELD(rq, u64, 0x180) = 0;
		MI_FIELD(rq, u64, 0x188) = 0;
		MI_FIELD(rq, u64, 0x190) = 0;
		MI_FIELD(rq, u64, 0x198) = 0;
		MI_FIELD(rq, u64, 0x1a0) = 0;
		MI_FIELD(rq, u64, 0x1a8) = 0;
		MI_FIELD(rq, u64, 0x1b0) = 0;
		INIT_LIST_HEAD(&MI_FIELD(rq, struct list_head, 0x1b8));
	}
	pr_info("metis-tp: metis task placement module init!");
	return 0;
}

/* 00122124 / .text+0x196fc..0x19720. */
void metis_tp_exit(void)
{
	pr_info("metis-tp: metis task placement module exit!");
}

static u64 first_wait_us(unsigned int source, unsigned int destination)
{
	struct mi_rq *rq = load_rq(source);
	struct list_head *head = &MI_FIELD(rq, struct list_head, 0x1b8);
	struct list_head *node = head->next;
	struct mi_task_struct *state;
	struct task_struct *task;
	if (node == head) {
		if (metis_lb_debug)
			pr_warn("metis-lb: metis_lb: vip tasks list is emtity!");
		return 0;
	}
	if (node == head || (unsigned long)node == 0x2a0) {
		if (metis_lb_debug)
			pr_warn("metis-lb: metis_lb: mi task struct is NULL!");
		return 0;
	}
	state = (struct mi_task_struct *)((u8 *)node - 0x2a0);
	task = state->task;
	/* Stock dereferences the task without checking it here. */
	if (!cpumask_test_cpu(destination, task->cpus_ptr))
		return 0;
	return (fas_sched_clock() - MI_FIELD(state, u64, 0x280)) / 1000;
}

/* 001214d0 / .text+0x18aa8..0x18ce4. */
int find_vip_runnable_longest_cpu(unsigned int destination,
		const struct cpumask *mask, int *reason)
{
	u32 bits = (u32)cpumask_bits(mask)[0];
	u64 longest = 0, wait;
	unsigned int cpu;
	int chosen = -1;
	while (bits) {
		cpu = __ffs(bits);
		bits &= bits - 1;
		if (cpu == destination || !MI_FIELD(load_rq(cpu), u32, 0x170) ||
			cpu_rq(cpu)->nr_running <= 1)
			continue;
		wait = first_wait_us(cpu, destination);
		if (wait > longest) {
			longest = wait;
			chosen = cpu;
		}
	}
	if (chosen < 0 || longest <= 3000)
		return -1;
	*reason = 1;
	if (metis_lb_debug)
		pr_info("metis-lb: metis_lb: select cpu%d, dst cpu%d, longest = %llu",
			chosen, destination, longest);
	return chosen;
}

/* 00121008 / .text+0x185e0..0x1888c. */
int find_vip_busiest_similar_cap_cpu(unsigned int destination,
		const struct cpumask *mask, int *reason)
{
	u32 bits = (u32)cpumask_bits(mask)[0];
	u64 longest = 0, wait;
	unsigned long busiest_load = 0, load;
	unsigned int cpu;
	int wait_cpu = -1, busiest_cpu = -1, chosen;
	while (bits) {
		cpu = __ffs(bits);
		bits &= bits - 1;
		if (cpu == destination || !MI_FIELD(load_rq(cpu), u32, 0x170) ||
			cpu_rq(cpu)->nr_running <= 1)
			continue;
		wait = first_wait_us(cpu, destination);
		if (wait > longest) {
			longest = wait;
			wait_cpu = cpu;
		}
		load = get_cpu_vip_tasks_load(cpu);
		if (load > busiest_load) {
			busiest_load = load;
			busiest_cpu = cpu;
		}
	}
	if (wait_cpu >= 0 && longest >= 3001) {
		chosen = wait_cpu;
		*reason = 1;
	} else if (busiest_cpu >= 0 && metis_lb_cpu_overutilized(busiest_cpu) &&
		MI_FIELD(load_rq(busiest_cpu), u32, 0x170) > 1) {
		chosen = busiest_cpu;
		*reason = 2;
	} else {
		return -1;
	}
	if (metis_lb_debug)
		pr_info("metis-lb: metis_lb: select cpu%d, dst cpu%d, longest = %llu, busiest_util = %lu",
			chosen, destination, longest, busiest_load);
	return chosen;
}

/* 00120d20 / .text+0x182f8..0x185e0. No new row/null-entry guards. */
int metis_lb_find_busiest_cpu(unsigned int cpu, int cluster, int *reason)
{
	const struct cpumask *mask;
	u32 bits;
	unsigned int first;
	bool higher, lower;
	int index, found;
	if (!mi_viptask_balance || !metis_lb_check_for_higher_capacity ||
		!metis_lb_cpu_array || !metis_lb_cpu_overutilized ||
		metis_lb_min_possible_cluster_id == -1 || !metis_lb_cpu_halt_mask ||
		cpumask_test_cpu(cpu, metis_lb_cpu_halt_mask) ||
		metis_wakeup_cold_start_skip_ui_cpu(NULL, cpu))
		return -1;
	if (cluster == metis_lb_min_possible_cluster_id)
		return find_vip_busiest_similar_cap_cpu(cpu,
			metis_lb_cpu_array[cluster][0], reason);
	for (index = 0; index < 3; index++) {
		mask = metis_lb_cpu_array[cluster][index];
		bits = (u32)cpumask_bits(mask)[0];
		first = bits ? __ffs(bits) : 32;
		higher = metis_lb_check_for_higher_capacity(first, cpu);
		lower = metis_lb_check_for_higher_capacity(cpu, first);
		found = !higher && !lower ?
			find_vip_busiest_similar_cap_cpu(cpu, mask, reason) :
			find_vip_runnable_longest_cpu(cpu, mask, reason);
		if (found >= 0)
			return found;
	}
	return -1;
}

static void destination_activate(struct rq *rq, struct task_struct *task)
{
	unsigned long flags;
	raw_spin_rq_lock_irqsave(rq, flags);
	activate_task(rq, task, 0);
	check_preempt_curr(rq, task, 0);
	raw_spin_rq_unlock_irqrestore(rq, flags);
}

/* 00120b50 / .text+0x18128..0x182f8: releases source before acquiring
 * destination, with no rq clock update. Fallback bypasses source removal. */
int pull_tasks_fast_path(unsigned int destination, unsigned int source,
		struct task_struct *fallback)
{
	struct rq *src, *dst;
	struct mi_rq *state;
	struct list_head *head, *node;
	struct mi_task_struct *task_state;
	struct task_struct *task = fallback;
	u64 wait = 0, age;
	unsigned long flags;
	load_cpu_bound(destination);
	load_cpu_bound(source);
	if (source == destination)
		return 0;
	src = cpu_rq(source);
	dst = cpu_rq(destination);
	state = load_rq(source);
	raw_spin_rq_lock_irqsave(src, flags);
	head = &MI_FIELD(state, struct list_head, 0x1b8);
	node = head->next;
	if (node != head && (unsigned long)node != 0x2a0) {
		task_state = (struct mi_task_struct *)((u8 *)node - 0x2a0);
		age = fas_sched_clock() - MI_FIELD(task_state, u64, 0x280);
		wait = age / 1000;
		if (task_state->task && !task_state->task->on_cpu &&
			cpumask_test_cpu(destination, task_state->task->cpus_ptr) &&
			age >= 0x2dcaa8) {
			task = task_state->task;
			deactivate_task(src, task, 0);
			set_task_cpu(task, cpu_of(dst));
		}
	}
	raw_spin_rq_unlock_irqrestore(src, flags);
	if (!task)
		return 0;
	destination_activate(dst, task);
	if (metis_lb_debug)
		pr_info("metis-lb: metis_lb: pull task fast, longest = %llu, s_cpu%d, d_cpu%d, pid = %d, name = %s",
			wait, source, destination, task->pid, task->comm);
	return 1;
}

/* 00120888 / .text+0x17e60..0x18128: reverse rb walk, at most six
 * eligible tasks. Original fallback activation defect is retained. */
int metis_lb_pull_tasks(unsigned int destination, unsigned int source,
		struct task_struct *fallback, int *reason)
{
	struct rq *src, *dst;
	struct mi_rq *state;
	struct rb_node *node;
	struct mi_task_struct *task_state;
	struct task_struct *task, *longest_task = NULL, *marked = NULL;
	u64 longest = 0, wait;
	unsigned int count = 0;
	unsigned long flags;
	if (!mi_viptask_balance || !metis_lb_cpu_halt_mask ||
		cpumask_test_cpu(destination, metis_lb_cpu_halt_mask) ||
		metis_wakeup_cold_start_skip_ui_cpu(NULL, destination))
		return 0;
	if (*reason == 1)
		return pull_tasks_fast_path(destination, source, fallback);
	if (*reason != 2)
		return 0;
	load_cpu_bound(destination);
	load_cpu_bound(source);
	if (source == destination)
		return 0;
	src = cpu_rq(source);
	dst = cpu_rq(destination);
	state = load_rq(source);
	raw_spin_rq_lock_irqsave(src, flags);
	node = rb_last(&MI_FIELD(state, struct rb_root_cached, 0x160).rb_root);
	for (; node; node = rb_prev(node)) {
		if ((unsigned long)node == 0x258)
			continue;
		task_state = (struct mi_task_struct *)((u8 *)node - 0x258);
		task = task_state->task;
		if (!task || !cpumask_test_cpu(destination, task->cpus_ptr) || task->on_cpu)
			continue;
		if (!marked && MI_FIELD(task_state, u8, 0xba))
			marked = task;
		wait = (fas_sched_clock() - MI_FIELD(task_state, u64, 0x280)) / 1000;
		count++;
		if (wait > longest) {
			longest = wait;
			longest_task = task;
		}
		if (count > 5)
			break;
	}
	task = longest_task && longest > 3000 ? longest_task :
		(marked && count > 1 ? marked : NULL);
	if (task) {
		deactivate_task(src, task, 0);
		set_task_cpu(task, cpu_of(dst));
	} else {
		task = fallback;
	}
	raw_spin_rq_unlock_irqrestore(src, flags);
	if (!task)
		return 0;
	destination_activate(dst, task);
	if (metis_lb_debug)
		pr_info("metis-lb: metis_lb: pull task slow, longest = %llu, s_cpu%d, d_cpu%d, pid = %d, name = %s",
			longest, source, destination, task->pid, task->comm);
	return 1;
}

/* 001212f8 / .text+0x188d0..0x18a84. Unused third formal is provided
 * by current canonical android_rvh_sched_rebalance_domains hook. */
void rebalance_vip_tasks(void *unused, struct rq *rq, int *continue_balancing)
{
	unsigned int cpu;
	int cluster, source, reason;
	if (!mi_viptask_balance || !rq || !metis_lb_cpu_array ||
		metis_lb_min_possible_cluster_id == -1 || !metis_lb_cpu_halt_mask)
		return;
	cpu = cpu_of(rq);
	if (cpumask_test_cpu(cpu, metis_lb_cpu_halt_mask) ||
		metis_wakeup_cold_start_skip_ui_cpu(NULL, cpu) ||
		!metis_lb_cpu_overutilized || metis_lb_cpu_overutilized(cpu) ||
		get_cpu_layered_load(cpu))
		return;
	load_cpu_bound(cpu);
	cluster = cpu_topology[cpu].cluster_id;
	if (cluster == -1)
		return;
	rcu_read_lock();
	source = find_vip_runnable_longest_cpu(cpu, metis_lb_cpu_array[cluster][0], &reason);
	if (source < 0 && cluster != metis_lb_min_possible_cluster_id) {
		source = find_vip_runnable_longest_cpu(cpu, metis_lb_cpu_array[cluster][1], &reason);
		if (source < 0)
			source = find_vip_runnable_longest_cpu(cpu, metis_lb_cpu_array[cluster][2], &reason);
	}
	if (source >= 0 && source != cpu)
		pull_tasks_fast_path(cpu, source, NULL);
	rcu_read_unlock();
}

/* 001212b4 / .text+0x1888c..0x188d0: ignores registration failure. */
int metis_lb_init(void)
{
	register_trace_android_rvh_sched_rebalance_domains(rebalance_vip_tasks, NULL);
	pr_info("metis-lb: metis load balance module init!");
	return 0;
}

/* 001214ac / .text+0x18a84..0x18aa8: stock does not unregister hook. */
void metis_lb_exit(void)
{
	pr_info("metis-lb: metis load balance module exit!");
}
