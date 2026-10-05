// SPDX-License-Identifier: GPL-2.0
/* Source-only, unwired recovery of migt task and CPU selection callbacks. */
#include <linux/arch_topology.h>
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/cred.h>
#include <linux/export.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include "migt.h"

int boost_policy, affinity_only, choose_cpu_exclusive_enable, custom_second_group;
int force_viptask_select_rq, gtask_override_schedboost;
int stask_prefer_cpu = -1, render_prefer_cpu = -1;
int vip_prefer_cpu = -1, ip_prefer_cpu = -1;
int stask_affine_cpu = -1, render_affine_cpu = -1;
int vip_affine_cpu = -1, ip_affine_cpu = -1;
int stask_prefer_cluster = -1, render_prefer_cluster = -1;
int vip_prefer_cluster = -1, ip_prefer_cluster = -1;
u64 migt_reserve_cpus, migt_reserve_cpus_rt, migt_reserve_cpus_mtk_vip;
u32 last_traced_uid;
int migt_thresh = 18, migt_viptask_thresh = 50, fps_variance_ratio = 10;
int fps_mexec_update_policy, sys_migt_count;
u64 migt_last_update_time;
bool migt_boost_flag, vtask_boost_flag;
u8 frame_load_has_init, game_load_has_init;
int flw_enable, flw_freq_enable, flw_debug;
/* Filled by the omitted real render worker's seqlock snapshot. */
u8 rinfo_back[0x80];

/* .text+4..0x20; zero is the actual disabled stock default. */
int migt_enable(void) { return boost_policy; }
int migt_affinity_only(void) { return affinity_only; }
int get_migt_thresh(void) { return migt_thresh; }
u32 get_cur_render_uid(void) { return last_traced_uid; }
bool package_runtime_should_stop(void) { return boost_policy == 0; }

/* .text+0x2bec..0x2c14: alias provider-owned storage, do not allocate. */
struct migt_task_state *get_gt_task(struct task_struct *task)
{
	struct mi_task_struct *mi = get_mi_task_struct(task);
	return mi ? (struct migt_task_state *)((u8 *)mi + 0x1a0) : NULL;
}
EXPORT_SYMBOL_GPL(get_gt_task);

static bool same_game_uid(struct task_struct *task)
{
	u32 uid;
	rcu_read_lock();
	/* Stock task+0x818 is objective real_cred, unlike io_monitor's cred. */
	uid = __kuid_val(rcu_dereference(task->real_cred)->uid);
	rcu_read_unlock();
	return uid == get_cur_render_uid();
}

unsigned long game_task(struct task_struct *task)
{
	return migt_enable() ? same_game_uid(task) : 0;
}

unsigned int is_render_thread(struct task_struct *task)
{
	struct migt_task_state *gt = get_gt_task(task);
	return gt ? GT_FLAGS(gt) & 0xc0 : 0;
}

bool game_render_task(struct task_struct *task)
{
	return is_render_thread(task) != 0;
}

unsigned long game_super_task(struct task_struct *task)
{
	struct migt_task_state *gt;
	if (!migt_enable() || !same_game_uid(task))
		return 0;
	gt = get_gt_task(task);
	return gt ? GT_FLAGS(gt) & 0x100 : 0;
}

bool game_ssuper_task(struct task_struct *task)
{
	return game_super_task(task) >> 8;
}

unsigned long is_uuser_set_vip(struct task_struct *task)
{
	struct migt_task_state *gt = get_gt_task(task);
	return migt_enable() && same_game_uid(task) && gt ? (GT_FLAGS(gt) >> 5) & 1 : 0;
}

unsigned long is_uuser_set_ip(struct task_struct *task)
{
	struct migt_task_state *gt = get_gt_task(task);
	return migt_enable() && same_game_uid(task) && gt ? (GT_FLAGS(gt) >> 3) & 1 : 0;
}

unsigned long game_vip_task(struct task_struct *task)
{
	struct migt_task_state *gt;
	u32 flags;
	if (!migt_enable() || !same_game_uid(task))
		return 0;
	gt = get_gt_task(task);
	if (!gt)
		return 0;
	flags = GT_FLAGS(gt);
	if (flags & 0x20)
		return 1;
	return flags & 8 ? 0 : flags & 0x1d0;
}

bool game_vvip_task(struct task_struct *task)
{
	return game_vip_task(task) != 0;
}

unsigned long game_ip_task(struct task_struct *task)
{
	struct migt_task_state *gt;
	if (!migt_enable() || !same_game_uid(task))
		return 0;
	gt = get_gt_task(task);
	return gt ? (GT_FLAGS(gt) & 0x28 ? 1 : !!(GT_FLAGS(gt) & 0x1d4)) : 0;
}

unsigned long game_task_override_schedboost(struct task_struct *task)
{
	struct migt_task_state *gt;
	if (!migt_enable() || !task || !gtask_override_schedboost)
		return 0;
	if (gtask_override_schedboost == 2)
		return 1;
	if (gtask_override_schedboost != 1 || !migt_enable() || !same_game_uid(task))
		return 0;
	gt = get_gt_task(task);
	return gt ? !!(GT_FLAGS(gt) & 0x1fc) : 0;
}

/* .text+0x488c..0x4a4c: render classification itself is not UID gated. */
int migt_choose_exclusive_cpu(struct task_struct *task)
{
	int preferred, affine;
	if (!migt_enable() || !choose_cpu_exclusive_enable || !migt_reserve_cpus ||
	    !game_task(task))
		return -1;
	if (game_ssuper_task(task)) {
		preferred = stask_prefer_cpu;
		affine = stask_affine_cpu;
	} else if (game_render_task(task)) {
		preferred = render_prefer_cpu;
		affine = render_affine_cpu;
	} else if (is_uuser_set_vip(task)) {
		preferred = vip_prefer_cpu;
		affine = vip_affine_cpu;
	} else if (is_uuser_set_ip(task)) {
		preferred = ip_prefer_cpu;
		affine = ip_affine_cpu;
	} else {
		return -1;
	}
	return preferred >= 0 ? preferred : affine;
}

static bool exclusion_enabled(void)
{
	return migt_enable() && choose_cpu_exclusive_enable && migt_reserve_cpus;
}

bool migt_skip_cpus(struct cpumask *mask)
{
	if (!exclusion_enabled())
		return false;
	cpumask_bits(mask)[0] = 0;
	cpumask_bits(mask)[0] = migt_reserve_cpus;
	return true;
}

bool migt_should_skip_this_cpu(unsigned int cpu)
{
	return exclusion_enabled() && test_bit(cpu, (unsigned long *)&migt_reserve_cpus);
}

bool migt_should_skip_this_cpu_rt(struct task_struct *task, unsigned int cpu)
{
	return migt_should_skip_this_cpu(cpu);
}

/* .text+0x4b1c..0x4e84: affine CPU only changes the outside-mask veto;
 * an inside-mask destination requires an exact preferred CPU. */
bool migt_should_skip_migrate(struct task_struct *task, unsigned int cpu)
{
	if (!exclusion_enabled())
		return false;
	if (!test_bit(cpu, (unsigned long *)&migt_reserve_cpus)) {
		if (task && game_task(task)) {
			if (game_render_task(task) && (render_prefer_cpu != -1 || render_affine_cpu != -1))
				return true;
			if (game_ssuper_task(task) && (stask_prefer_cpu != -1 || stask_affine_cpu != -1))
				return true;
			if (is_uuser_set_vip(task) && (vip_prefer_cpu != -1 || vip_affine_cpu != -1))
				return true;
			if (is_uuser_set_ip(task) && (ip_prefer_cpu != -1 || ip_affine_cpu != -1))
				return true;
		}
		return false;
	}
	if (task && game_task(task)) {
		if (game_render_task(task) && render_prefer_cpu != -1 && render_prefer_cpu == cpu)
			return false;
		if (game_ssuper_task(task) && stask_prefer_cpu != -1 && stask_prefer_cpu == cpu)
			return false;
		if (is_uuser_set_vip(task) && vip_prefer_cpu != -1 && vip_prefer_cpu == cpu)
			return false;
		if (is_uuser_set_ip(task) && ip_prefer_cpu != -1 && ip_prefer_cpu == cpu)
			return false;
	}
	return true;
}

void migt_skip_lowest_rt(struct cpumask *mask)
{
	u64 old;
	u32 remaining;
	if (!exclusion_enabled())
		return;
	old = cpumask_bits(mask)[0];
	remaining = (u32)old & ~(u32)migt_reserve_cpus_rt;
	cpumask_bits(mask)[0] = remaining;
	if (!remaining)
		cpumask_bits(mask)[0] = old;
}

void migt_skip_mtk_vip_cpus(struct cpumask *mask)
{
	if (exclusion_enabled()) {
		cpumask_bits(mask)[0] = 0;
		cpumask_bits(mask)[0] = migt_reserve_cpus_mtk_vip;
	}
}

bool migt_should_skip_mtk_affinity_task(void)
{
	return exclusion_enabled() && (u32)migt_reserve_cpus_mtk_vip != 0;
}

bool migt_add_task_to_second(int pid)
{
	struct task_struct *task;
	bool found = false;
	if (pid < 0 || !exclusion_enabled() || !custom_second_group)
		return false;
	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task)
		found = is_uuser_set_ip(task) || is_uuser_set_vip(task);
	rcu_read_unlock();
	return found;
}

bool game_choose_cpu_overutiled(struct task_struct *task)
{
	if (!task || !migt_enable() || !force_viptask_select_rq)
		return false;
	if (game_ssuper_task(task) && stask_prefer_cluster >= 0)
		return true;
	if (game_render_task(task) && render_prefer_cluster >= 0)
		return true;
	if (force_viptask_select_rq < 2) {
		if (is_uuser_set_vip(task) && vip_prefer_cluster >= 0)
			return true;
		if (!is_uuser_set_ip(task))
			return false;
	} else {
		if (game_vvip_task(task) && vip_prefer_cluster >= 0)
			return true;
		if (!game_task(task))
			return false;
		{
			struct migt_task_state *gt = get_gt_task(task);
			if (!gt || !(GT_FLAGS(gt) & 0x1fc))
				return false;
		}
	}
	return ip_prefer_cluster >= 0;
}

void update_migt_reserve_cpus(void)
{
	migt_reserve_cpus = 0;
	if (stask_prefer_cpu >= 0)
		set_bit(stask_prefer_cpu, (unsigned long *)&migt_reserve_cpus);
	if (render_prefer_cpu >= 0)
		set_bit(render_prefer_cpu, (unsigned long *)&migt_reserve_cpus);
	if (vip_prefer_cpu >= 0)
		set_bit(vip_prefer_cpu, (unsigned long *)&migt_reserve_cpus);
	if (ip_prefer_cpu >= 0)
		set_bit(ip_prefer_cpu, (unsigned long *)&migt_reserve_cpus);
}

/* .text+0x24..0xd0: add same-cluster possible CPUs, retaining CPU32 boundary. */
void mi_cpu_coregroup_mask(unsigned int cpu, struct cpumask *mask)
{
	unsigned int other;
	BUG_ON(cpu > 32);
	if (cpu_topology[cpu].cluster_id == -1)
		return;
	for_each_possible_cpu(other) {
		if (other >= 32)
			break;
		if (cpu_topology[other].cluster_id == cpu_topology[cpu].cluster_id)
			cpumask_set_cpu(other, mask);
	}
}

u8 flw_update_freq_enable(void)
{
	return migt_enable() && flw_enable && flw_freq_enable ? frame_load_has_init : 0;
}

unsigned long frame_load_enable(void)
{
	return migt_enable() && flw_enable ? frame_load_has_init : 0;
}

unsigned long is_rcal_task(struct task_struct *task, int caller_flag)
{
	struct migt_task_state *gt;
	if (!migt_enable() || frame_load_has_init != 1 || !flw_enable)
		return 0;
	gt = get_gt_task(task);
	return gt ? ((!(GT_FLAGS(gt) & 0x800) || caller_flag != 0) & (GT_FLAGS(gt) >> 10)) : 0;
}

/* .text+0x32bc..0x3388: prefix comparison, including zero-length match. */
static bool is_render_sibling(struct task_struct *task)
{
	unsigned int i;
	size_t length;
	for (i = 0; i < 2; i++) {
		if (!rinfo_back[i * 0x40 + 8])
			continue;
		length = strnlen((char *)&rinfo_back[i * 0x40 + 0x18], 16);
		/* Stock uses fortify_panic(__fortify_strlen) for an unterminated name. */
		BUG_ON(length == 16);
		if (!strncmp((char *)&rinfo_back[i * 0x40 + 0x18], task->comm, length))
			return true;
	}
	return false;
}

bool is_frender_sibling(struct task_struct *task)
{
	return is_render_sibling(task);
}
