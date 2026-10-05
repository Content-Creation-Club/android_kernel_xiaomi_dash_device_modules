// SPDX-License-Identifier: GPL-2.0
/* Stock frequency producers and seven-argument callback, still unregistered. */
#include <linux/bug.h>
#include "kernel/sched/sched.h"
#include "migt.h"

int glk_disable = 1, glk_fbreak_enable, glk_freq_limit_walt, glk_freq_limit_start;
bool glk_maxfreq_limit = true, glk_minfreq_limit = true;
u8 force_maxfreq_break;
u32 glk_maxfreq[3], glk_minfreq[3];
int glk_highspeed_load[3] = { 90, 90, 90 };
int glk_lowspeed_load[3] = { 30, 30, 30 };
int game_load_debug, game_load_perf_mod;
u32 cur_history_items;
u64 game_load_last_update_time;
struct migt_game_history game_load_history[21];

extern u64 fas_sched_clock(void);
extern unsigned int cpu_to_cluster(unsigned int cpu);
/* Real metis-owned exported objects; no substitute definition is supplied. */
extern int in_perf_mod;
extern u32 perf_min_freq[3];
u32 cur_record_freq[3];
u64 last_freq_change_time[3], sum_freq_rt[3];

/* .text+0x5f90..0x6034; current native restricted hook takes a policy. */
void android_rvh_cpufreq_transition(void *unused, struct cpufreq_policy *policy)
{
	u32 cluster = cpu_to_cluster(policy->cpu);
	u64 last;
	BUG_ON(cluster >= 3);
	last = last_freq_change_time[cluster];
	sum_freq_rt[cluster] -= last * cur_record_freq[cluster];
	last_freq_change_time[cluster] = 0;
	cur_record_freq[cluster] = policy->cur;
	if (flw_debug == 4)
		pr_info("flw: [%d]%llu, %d\n", cluster, -last, policy->cur);
}

bool glk_enable(void)
{
	return !glk_disable && fas_sched_clock() - game_load_last_update_time < 1000000000 &&
		migt_enable() > 2;
}

void glk_force_maxfreq_break(u8 value) { force_maxfreq_break = value; }
void glk_maxfreq_break(u8 value)
{
	if (glk_fbreak_enable)
		glk_maxfreq_limit = (~value) & 1;
}
void glk_minfreq_break(u8 value)
{
	if (glk_fbreak_enable)
		glk_minfreq_limit = (~value) & 1;
}

/* .text+0x6ef8..0x736c; producer KCFI hash0x428fdb77, source type UNTESTED. */
void set_next_freq(void *data, unsigned long util, unsigned long mapped_freq,
		unsigned long max, unsigned long *freq, struct cpufreq_policy *policy,
		bool *need_update)
{
	u64 old, result, load, ratio, period, capacity;
	u32 index, cluster, upper, lower, chosen;
	int enabled;
	if (game_load_has_init != 1)
		return;
	old = *freq;
	if (migt_affinity_only())
		return;
	enabled = migt_enable();
	index = cur_history_items;
	if (!enabled) {
		if (!in_perf_mod) {
			result = *freq;
			goto compare;
		}
		BUG_ON(policy->cpu >= 32);
		cluster = per_cpu(migt_game_load_info, policy->cpu).cluster;
		BUG_ON(cluster > 2);
		result = max(policy->min, perf_min_freq[cluster]);
		if (game_load_debug)
			pr_info("glk: min_freq %lu\n", (unsigned long)result);
		result = max(result, (u64)*freq);
		goto done;
	}
	if (glk_disable || fas_sched_clock() - game_load_last_update_time > 999999999 ||
	    migt_enable() < 3 || game_load_has_init != 1)
		goto fallback;
	index = index ? index - 1 : 20;
	BUG_ON(index > 21);
	/* Stock's second guard traps index21 after the first admits it. */
	BUG_ON(((u64)index << 6 | 0x20) > 0x540);
	BUG_ON(policy->cpu >= 32);
	cluster = per_cpu(migt_game_load_info, policy->cpu).cluster;
	BUG_ON(cluster > 2);
	BUG_ON((u64)index * 0x40 + cluster * 8 > 0x53f);
	upper = glk_maxfreq_limit ? glk_maxfreq[cluster] : policy->max;
	lower = glk_minfreq_limit ? glk_minfreq[cluster] : policy->min;
	period = game_load_history[index].period;
	/* Preserve stock's reversed bound test: lower < upper takes fallback. */
	if (lower < upper || period < 1000000)
		goto fallback;
	/* Semantic writer/consumer mapping, not a cast through stock rq offsets. */
	capacity = cpu_rq(policy->cpu)->cpu_capacity_orig;
	if (!glk_minfreq[cluster])
		glk_minfreq[cluster] = policy->min;
	load = game_load_history[index].load[cluster];
	if ((u64)(u32)get_migt_thresh() * 2 < period / 1000000) {
		result = lower;
		if (policy->cur >= lower && game_load_perf_mod)
			result = policy->max;
	} else {
		ratio = capacity ? load * 100 / capacity : 0;
		result = lower;
		if (ratio <= (u32)glk_highspeed_load[cluster]) {
			result = upper;
			if (ratio >= (u32)glk_lowspeed_load[cluster]) {
				chosen = (u32)(ratio * (u32)(lower + (lower >> 3)) / 100);
				lower = min(lower, chosen);
				upper = max(upper, lower);
				result = upper;
				if (game_load_debug)
					pr_info("glk: glk_load %llu, target_freq %d\n", ratio, (u32)result);
			}
		}
	}
	if ((u32)result)
		goto updated;
fallback:
	result = (u32)*freq;
	if (glk_freq_limit_walt && glk_freq_limit_start && game_load_has_init == 1) {
		BUG_ON(policy->cpu >= 32);
		cluster = per_cpu(migt_game_load_info, policy->cpu).cluster;
		if (glk_maxfreq_limit && !force_maxfreq_break) {
			BUG_ON(cluster > 2);
			upper = glk_maxfreq[cluster];
		} else {
			upper = policy->max;
		}
		if (glk_minfreq_limit) {
			BUG_ON(cluster > 2);
			lower = glk_minfreq[cluster];
		} else {
			lower = policy->min;
		}
		lower = max(policy->min, lower);
		if (!upper)
			upper = policy->max;
		result = min(upper, max(lower, (u32)*freq));
	}
updated:
	/* Stock calls this helper and discards its return. */
	flw_update_freq_enable();
done:
	*freq = result;
compare:
	if (result != old)
		*need_update = true;
}
