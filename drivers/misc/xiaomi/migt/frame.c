// SPDX-License-Identifier: GPL-2.0
/* Stock frame reset and setters. No missing frame runtime updater is synthesized. */
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "migt.h"

#define FRAME_FIELD(record, type, offset) (*(type *)((record)->raw + (offset)))
struct migt_frame_history frame_load_history[21];
DEFINE_PER_CPU(struct migt_frame_cpu, migt_frame_load_info);
int migt_frame_cluster_number;
u64 migt_frame_cluster_cpumask[3];
u32 target_delta = 1650, old_target_delta;
u64 migt_frame_period_ns;
int need_to_base, need_to_update, need_to_record;

/* .text+0x5864..0x5b9c. Only fields cleared by stock are touched. */
void frame_load_reset(void)
{
	u64 period = migt_frame_period_ns, new_period;
	u32 old_delta = old_target_delta, delta = target_delta, mask;
	int index, field, cluster, clusters;
	unsigned int cpu;

	if (!migt_enable() || frame_load_has_init != 1 || !flw_enable)
		return;
	new_period = (u64)delta * 10000;
	if (old_delta != delta || !period) {
		old_target_delta = delta;
		migt_frame_period_ns = new_period;
		period = new_period;
	}
	for (index = 0; index < 21; ++index) {
		struct migt_frame_history *record = &frame_load_history[index];
		for (field = 0x08; field <= 0x38; field += 8)
			FRAME_FIELD(record, u64, field) = 0;
		/* Stock's first record clears 64 bits here; subsequent ones clear 32. */
		if (!index)
			FRAME_FIELD(record, u64, 0x40) = 0;
		else
			FRAME_FIELD(record, u32, 0x40) = 0;
		memset(record->raw + 0x48, 0, 0x1c0);
		for (field = 0x208; field <= 0x230; field += 8)
			FRAME_FIELD(record, u64, field) = 0;
		FRAME_FIELD(record, u64, 0x2d0) = 0;
		FRAME_FIELD(record, u64, 0x2d8) = 0;
		FRAME_FIELD(record, u64, 0x2e8) = 0;
		FRAME_FIELD(record, u64, 0x2f0) = 0;
		if (index && !period) {
			old_target_delta = delta;
			migt_frame_period_ns = new_period;
			period = new_period;
		}
		FRAME_FIELD(record, u32, 0) = period >> 10;
		FRAME_FIELD(record, u64, 0x2e0) = period;
		FRAME_FIELD(record, u32, 0x350) = 0;
	}
	clusters = migt_frame_cluster_number;
	for (cluster = 0; cluster < clusters; ++cluster) {
		BUG_ON(cluster == 3);
		mask = (u32)migt_frame_cluster_cpumask[cluster];
		while (mask) {
			struct migt_frame_cpu *info;
			cpu = __ffs(mask);
			mask &= ~(1U << cpu);
			info = &per_cpu(migt_frame_load_info, cpu);
			/* Stock frame reset has no CPU lock. */
			for (field = 0; field < 7; ++field) {
				info->counters[field] = 0;
				info->previous[field] = 0;
			}
		}
	}
	memset(cur_record_freq, 0, sizeof(cur_record_freq));
	memset(last_freq_change_time, 0, sizeof(last_freq_change_time));
	memset(sum_freq_rt, 0, sizeof(sum_freq_rt));
	need_to_base = need_to_update = need_to_record = 0;
	if (flw_debug == 1)
		pr_info("flw: frame load reset!!!!\n");
}

/* .text+0x5738..0x57e4; clear wins if the masked bit indices are equal. */
void set_flw_flag(struct task_struct *task, u32 set_bit, u32 clear_bit)
{
	struct migt_task_state *gt;
	if (!migt_enable() || frame_load_has_init != 1 || !flw_enable)
		return;
	gt = get_gt_task(task);
	if (gt)
		GT_FLAGS(gt) = (GT_FLAGS(gt) | (1U << (set_bit & 31))) &
			~(1U << (clear_bit & 31));
	if (flw_debug == 2)
		pr_info("flw: %16s-%5d set %d\n", task->comm, task->pid, set_bit);
}

/* .text+0x60b0..0x6134. Preserve sscanf syntax and reset-before-disable order. */
int set_flw_enable(const char *value, const struct kernel_param *param)
{
	u32 enabled = 0;
	if (sscanf(value, "%u\n", &enabled) != 1)
		return -EINVAL;
	if (!enabled)
		frame_load_reset();
	flw_enable = enabled;
	return 0;
}

/* .text+0x6138..0x6164. */
int get_flw_enable(char *buffer, const struct kernel_param *param)
{
	return sprintf(buffer, "%u\n", (u32)flw_enable);
}
