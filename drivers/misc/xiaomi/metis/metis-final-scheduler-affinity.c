// SPDX-License-Identifier: GPL-2.0-only
/* Stock cluster-affinity algorithms. Native task/cred/topology members;
 * Xiaomi's own state keeps its witnessed offsets. All integration UNTESTED. */
#include <linux/arch_topology.h>
#include <linux/bug.h>
#include <linux/capability.h>
#include <linux/cpuset.h>
#include <linux/cred.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <asm/thread_info.h>
#include <trace/hooks/cgroup.h>
#include <trace/hooks/sched.h>
#include "metis-final-scheduler.h"

static unsigned long affinity_word(const struct cpumask *mask)
{
	return cpumask_bits(mask)[0];
}

static bool affinity_compat(struct task_struct *task)
{
	return test_tsk_thread_flag(task, TIF_32BIT);
}

/* The original performs an unchecked low-word write bracketed by two
 * compat tests, and optionally BUGs if the thread changes personality. */
static unsigned long affinity_store(struct task_struct *task,
				   unsigned long *word, unsigned long value)
{
	unsigned long old = *word;

	if (affinity_compat(task))
		return old;
	*word = value;
	if (affinity_compat(task)) {
		if (bug_detect)
			BUG();
		*word = old;
		return old;
	}
	return value;
}

/* Original 0010cd14, .text+0x42ec..0x4398; CFI 7fe3444d. */
void mi_cpu_coregroup_mask(unsigned int cpu, struct cpumask *mask)
{
	unsigned int other;

	/* Stock admits CPU 32 and rejects only >=33. Preserve that boundary. */
	if (cpu > 32) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	if (cpu_topology[cpu].cluster_id == -1)
		return;
	for_each_possible_cpu(other) {
		if (other >= 32)
			break;
		if (cpu_topology[cpu].cluster_id == cpu_topology[other].cluster_id)
			cpumask_set_cpu(other, mask);
	}
}

/* Original 0010cdc4, .text+0x439c..0x43f4. Native cgroup.h supplies the
 * ignored probe-data formal and the child/inherit callback contract. */
void mi_cpuset_fork(void *unused, struct task_struct *child, bool *inherit)
{
	struct mi_task_struct *state = get_mi_task_struct(current);
	struct cpumask saved;

	if (!state || !(state->flags_0xc0 & 2)) {
		*inherit = false;
		return;
	}
	cpumask_clear(&saved);
	cpumask_bits(&saved)[0] = MI_FIELD(state, unsigned long, 0xd0);
	*inherit = true;
	set_cpus_allowed_ptr(child, &saved); /* Stock ignores errors. */
}

/* Original 0010d104, .text+0x46dc..0x4824; byte/word accesses and unsigned
 * UID bitmap bound are retained. A flag result may be 4, not only 1. */
unsigned int unbind_cpu_affinity_task(struct task_struct *task)
{
	struct mi_task_struct *state;
	unsigned int uid;
	bool listed;

	if (!task || !refcount_read(&task->usage))
		return 0;
	state = get_mi_task_struct(task);
	if (!state || affinity_compat(task))
		return 0;
	rcu_read_lock();
	uid = __kuid_val(__task_cred(task)->uid);
	rcu_read_unlock();
	listed = minor_window_app && minor_window_app == uid;
	if (!listed && (uid >> 6) < 3125)
		listed = test_bit(uid, cluster_affinity_uid_mlist) ||
			 test_bit(uid, cluster_affinity_uid_llist) ||
			 test_bit(uid, cluster_affinity_uid_blist);
	if (listed) {
		if (tsched_debug)
			pr_info("metis-clu-aff: uid is %s",
				uid_type(uid) ? "in uidlist" : "minor_window_app");
		return 1;
	}
	if (tsched_debug && (state->flags_0xc0 & 4))
		pr_info("metis-clu-aff: pid %d comm %s is rebind task\n",
			task->pid, task->comm);
	return state->flags_0xc0 & 4;
}

/* Original 0010ce20, .text+0x43f8..0x46dc; native sched.h proves every
 * callback formal, including unused data. No successful affinity syscall
 * substitute is invented: this hook only writes the driver's saved word. */
void mi_sched_setaffinity(void *unused, struct task_struct *task,
		const struct cpumask *mask, bool *skip)
{
	struct mi_task_struct *state;
	struct cpumask allowed, requested;
	const struct cred *cred;
	kuid_t uid = current_euid();
	bool same;
	unsigned long value;

	get_task_struct(task);
	state = get_mi_task_struct(task);
	if (!state || (task->flags & PF_NO_SETAFFINITY))
		goto out;
	rcu_read_lock();
	cred = __task_cred(task);
	same = uid_eq(uid, cred->euid) || uid_eq(uid, cred->uid);
	rcu_read_unlock();
	if (!same) {
		rcu_read_lock();
		same = ns_capable(__task_cred(task)->user_ns, CAP_SYS_NICE);
		rcu_read_unlock();
		if (!same)
			goto out;
	}
	if (tsched_debug) {
		pr_info("metis-clu-aff: debug_setaffinity: current : %s %d %d, setaffinity target : %s %d %d\n",
			current->comm, __kuid_val(current_uid()), current->pid,
			task->comm, __kuid_val(task_uid(task)), task->pid);
		pr_info("metis-clu-aff: debug_setaffinity: cpumask is : %*pbl \n",
			cpumask_pr_args(mask));
	}
	if (!unbind_cpu_affinity_task(task))
		goto out;
	cpuset_cpus_allowed(task, &allowed);
	cpumask_clear(&requested);
	value = (u32)affinity_word(mask) & (u32)affinity_word(&allowed);
	cpumask_bits(&requested)[0] = value;
	if (cpumask_any_and(&requested, cpu_active_mask) < nr_cpu_ids) {
		cpuset_cpus_allowed(task, &allowed);
		if (value & ~affinity_word(&allowed))
			value = affinity_word(&allowed);
	}
	cpumask_bits(&requested)[0] = value;
	affinity_store(state->task, &MI_FIELD(state, unsigned long, 0xd0), value);
	*skip = true;
	if (tsched_debug)
		pr_info("metis-clu-aff: debug_setaffinity: newmask is %*pbl",
			cpumask_pr_args(&requested));
out:
	put_task_struct(task);
}

/* Original 0010d2c8, .text+0x48a0..0x48e4; native hook new_mask is const. */
void mi_set_cpus_allowed_comm(void *unused, struct task_struct *task,
		const struct cpumask *mask)
{
	struct mi_task_struct *state = get_mi_task_struct(task);

	if (state) {
		state->flags_0xc0 &= ~2U;
		MI_FIELD(state, unsigned long, 0xd0) = affinity_word(mask);
	}
}

/* Original 0010d310, .text+0x48e8..0x4d98, CFI ab5097ae. Stock provider
 * mtk_select_task_rq_rt at00122914 passes task/excluded and the preserved
 * native int source_cpu, sd_flag, flags, int *target; it tests returned W0.
 * Source/SD formals are unused by this algorithm, not removed from the ABI. */
bool mi_select_task_rq_hook(struct task_struct *task, struct cpumask *excluded,
		int source_cpu, int sd_flag, int wake_flags, int *cpu)
{
	struct mi_task_struct *state, *selected_state;
	unsigned long value;
	unsigned int uid;
	int group, selected;
	bool done = false;

	if (!task || !refcount_read(&task->usage))
		return false;
	get_task_struct(task); /* Stock reads usage before incrementing it. */
	state = get_mi_task_struct(task);
	if (!state)
		goto out;
	if (!unbind_cpu_affinity_task(task)) {
		if (state->flags_0xc0 & 2) {
			state->flags_0xc0 &= ~2U;
			value = affinity_store(task, &cpumask_bits(&task->cpus_mask)[0],
				MI_FIELD(state, unsigned long, 0xd0));
			task->nr_cpus_allowed = hweight32(value);
		}
	} else if (!(state->flags_0xc0 & 2)) {
		state->flags_0xc0 |= 2;
		affinity_store(state->task, &MI_FIELD(state, unsigned long, 0xd0),
			       affinity_word(&task->cpus_mask));
		rcu_read_lock();
		uid = __kuid_val(__task_cred(task)->uid);
		rcu_read_unlock();
		selected_state = get_mi_task_struct(task);
		if (cluster_core_init_suc != 1 || !selected_state || affinity_compat(task))
			goto clear;
		if (minor_window_app && minor_window_app == uid) {
			value = affinity_word(cpu_possible_mask);
		} else if (selected_state->flags_0xc0 & 4) {
			value = MI_FIELD(selected_state, unsigned long, 0xd8);
			if (tsched_debug) {
				struct cpumask saved;

				cpumask_clear(&saved);
				cpumask_bits(&saved)[0] = value;
				pr_info("metis-clu-aff: pid %d comm %s get rebind mask: %*pbl",
					task->pid, task->comm, cpumask_pr_args(&saved));
			}
		} else {
			if ((uid >> 6) >= 3125)
				goto clear;
			if (test_bit(uid, cluster_affinity_uid_mlist))
				group = 2;
			else if (test_bit(uid, cluster_affinity_uid_llist))
				group = 1;
			else if (test_bit(uid, cluster_affinity_uid_blist))
				group = 3;
			else
				goto clear;
			value = affinity_word(&cluster_affinity_cpumask[group]);
		}
		/* Stock counts the requested word even if the second compat test
 		 * restores the old mask. It does not refresh task->cpus_ptr. */
		affinity_store(task, &cpumask_bits(&task->cpus_mask)[0], value);
		task->nr_cpus_allowed = hweight32(value);
		goto selected;
clear:
		state->flags_0xc0 &= ~2U;
	}
selected:
	if (tsched_debug)
		pr_info("metis-clu-aff: debug_setaffinity: cur: %s %d, cpumask: %*pbl %d.",
			task->comm, task->pid, cpumask_pr_args(&task->cpus_mask),
			task->nr_cpus_allowed);
	if (game_wake_hooks)
		game_wake_hooks(task, wake_flags);
	selected = metis_ui_and_ui_pipeline_select_rq(task, excluded, wake_flags);
	if (selected >= 0) {
		*cpu = selected;
		done = true;
	}
out:
	put_task_struct(task);
	return done;
}

/* Original 0010d7c4, .text+0x4d9c..0x51ac. All hook/sysctl results are
 * ignored; success is published even if topology gives no valid groups. */
int clu_aff_init(void)
{
	struct cpumask group;
	unsigned int cpu, printed;
	unsigned int cluster = 0, affinity = 1;
	unsigned long remaining = (u32)affinity_word(cpu_possible_mask), accumulated = 0;

	for_each_possible_cpu(cpu) {
		if (cpu >= 32)
			break;
		if ((int)cpu <= vip_task_max_cpu_nr)
			cpumask_set_cpu(cpu, &viptask_use_cpumask);
	}
	clu_aff_header = register_sysctl_sz("clu-aff/viptask_maxcpu_nr",
			clu_aff_table, 2);
	pr_info("metis-clu-aff: come into %s\n", "cluster_core_init");
	for (cpu = 0; cpu < 32 && cluster <= 2; cpu++) {
		if (!(remaining & BIT(cpu)))
			continue;
		cpumask_clear(&group);
		mi_cpu_coregroup_mask(cpu, &group);
		if (cpumask_first(&group) != cpu)
			continue;
		cpumask_bits(&cluster_core_mask[cluster])[0] = affinity_word(&group);
		pr_info("metis-clu-aff: **cluster**%d**\n", affinity);
		for_each_cpu(printed, &cluster_core_mask[cluster]) {
			if (printed >= 32)
				break;
			pr_info("metis-clu-aff: -%d \t", printed);
		}
		pr_info("metis-clu-aff: \n");
		accumulated |= affinity_word(&group);
		if (affinity < 4) {
			cpumask_bits(&cluster_affinity_cpumask[affinity])[0] = accumulated;
			pr_info("metis-clu-aff: clus_affnity %d ", affinity);
			for_each_cpu(printed, &cluster_affinity_cpumask[affinity]) {
				if (printed >= 32)
					break;
				pr_info("metis-clu-aff: -%d \t", printed);
			}
			pr_info("metis-clu-aff: \n");
		}
		remaining &= ~affinity_word(&group);
		++cluster;
		++affinity;
	}
	cpumask_bits(&cluster_affinity_cpumask[3])[0] =
		(u32)affinity_word(&cluster_affinity_cpumask[3]) &
		~(u32)affinity_word(&cluster_affinity_cpumask[1]);
	cluster_core_init_suc = 1;
	register_trace_android_rvh_cpuset_fork(mi_cpuset_fork, NULL);
	register_trace_android_vh_sched_setaffinity_early(mi_sched_setaffinity, NULL);
	register_trace_android_rvh_set_cpus_allowed_comm(mi_set_cpus_allowed_comm, NULL);
	pr_err("metis-clu-aff: clu_aff %s: inited!\n", "clu_aff_init");
	return 0;
}

/* Original 0010dbd8, .text+0x51b0..0x51e0. No hook unregister or quiesce. */
void clu_aff_exit(void)
{
	unregister_sysctl_table(clu_aff_header);
	clu_aff_header = NULL;
}
