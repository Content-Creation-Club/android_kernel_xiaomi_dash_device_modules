// SPDX-License-Identifier: GPL-2.0
/* Stock game history producers, unwired until the real lifecycle is recovered. */
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/string.h>
#include "migt.h"

int migt_game_cluster_number;
u64 migt_game_cluster_cpumask[3];
u32 sysctl_game_load_choose_policy;
u64 cpu_util_freq_divisor;
static int old_migt_thresh_game;
static u32 migt_game_frame_tperiod;
/* Stock .rodata+0x958, indexed by threshold 11..33. */
static const u32 game_period_table[23] = {
	11, 11, 11, 11, 11, 11, 16, 16, 16, 16, 16, 16,
	16, 16, 25, 25, 25, 25, 25, 25, 25, 25, 25,
};
extern u64 fas_sched_clock(void);

static u64 drain_counter(u64 *counter, u64 *previous)
{
	u64 value = *counter;
	*counter = 0;
	*previous = value;
	return value;
}

/* .text+0x64c4..0x68c0. Stock drains without taking the runtime writer's lock. */
void game_load_history_update(void)
{
	u64 now = fas_sched_clock(), selected, totals[7];
	struct migt_game_history *record;
	u32 index, old_index, policy, mask;
	int threshold, cluster, clusters;
	unsigned int cpu;

	if (game_load_has_init != 1)
		return;
	game_load_last_update_time = now;
	threshold = get_migt_thresh();
	index = cur_history_items;
	BUG_ON(index > 21);
	BUG_ON(((u64)index * 0x40 | 0x18) > 0x540);
	record = &game_load_history[index];
	if (!record->timestamp) {
		record->timestamp = now;
		return;
	}
	record->period = now - record->timestamp;
	policy = sysctl_game_load_choose_policy;
	clusters = migt_game_cluster_number;
	if (old_migt_thresh_game != threshold || !migt_game_frame_tperiod) {
		old_migt_thresh_game = threshold;
		migt_game_frame_tperiod = (u32)threshold - 11U < 23 ?
			game_period_table[(u32)threshold - 11U] : 33;
	}
	/* The shift wraps at 32 bits before becoming the 64-bit divisor. */
	cpu_util_freq_divisor = (u32)(migt_game_frame_tperiod << 10);
	old_index = index;
	if (clusters < 1) {
		index = old_index + 1 < 21 ? old_index + 1 : old_index - 20;
		cur_history_items = index;
		BUG_ON(index > 20);
	} else {
		for (cluster = 0; cluster < clusters; ++cluster) {
			BUG_ON(cluster == 3);
			mask = (u32)migt_game_cluster_cpumask[cluster];
			memset(totals, 0, sizeof(totals));
			while (mask) {
				struct migt_game_cpu *info;
				u64 all;
				cpu = __ffs(mask);
				mask &= ~(1U << cpu);
				info = &per_cpu(migt_game_load_info, cpu);
				all = drain_counter(&info->all, &info->all_last);
				totals[0] = max(totals[0], all);
				totals[1] += drain_counter(&info->game, &info->game_last);
				totals[2] += drain_counter(&info->super, &info->super_last);
				totals[3] += drain_counter(&info->vip, &info->vip_last);
				totals[4] += drain_counter(&info->render, &info->render_last);
				totals[5] += drain_counter(&info->counter_50, &info->counter_58);
				totals[6] += drain_counter(&info->counter_60, &info->counter_68);
			}
			selected = policy < 5 ? totals[policy] : totals[2];
			/* Truncate to signed 32 bits, then sign extend into the u64 slot. */
			record->load[cluster] = (s64)(s32)(cpu_util_freq_divisor ?
				selected / cpu_util_freq_divisor : 0);
		}
		index = (s32)(old_index + 1) % 21;
		cur_history_items = index;
		BUG_ON(index > 20);
		for (cluster = 0; cluster < clusters; ++cluster)
			game_load_history[index].load[cluster] = 0;
	}
	game_load_history[index].period = 0;
	game_load_history[index].timestamp = now;
}

/* .text+0x68c4..0x6acc. Unlike history update, reset takes each CPU lock. */
void game_load_reset(void)
{
	int index, cluster, clusters = migt_game_cluster_number;
	u32 mask;
	unsigned int cpu;
	unsigned long flags;

	if (game_load_has_init != 1)
		return;
	for (index = 0; index < 21; ++index) {
		game_load_history[index].timestamp = 0;
		game_load_history[index].period = 0;
		for (cluster = 0; cluster < clusters; ++cluster) {
			BUG_ON(cluster == 3);
			game_load_history[index].load[cluster] = 0;
		}
	}
	cur_history_items = 0;
	if (clusters < 1)
		return;
	/* Stock re-reads the live cluster count after each cluster's CPU reset. */
	for (cluster = 0; ; ++cluster) {
		BUG_ON(cluster == 3);
		mask = (u32)migt_game_cluster_cpumask[cluster];
		while (mask) {
			struct migt_game_cpu *info;
			cpu = __ffs(mask);
			mask &= ~(1U << cpu);
			info = &per_cpu(migt_game_load_info, cpu);
			spin_lock_irqsave(&info->lock, flags);
			info->all = info->all_last = 0;
			info->game = info->game_last = 0;
			info->super = info->super_last = 0;
			info->vip = info->vip_last = 0;
			info->render = info->render_last = 0;
			info->counter_50 = info->counter_58 = 0;
			info->counter_60 = info->counter_68 = 0;
			spin_unlock_irqrestore(&info->lock, flags);
		}
		if (migt_game_cluster_number <= cluster + 1)
			return;
	}
}
