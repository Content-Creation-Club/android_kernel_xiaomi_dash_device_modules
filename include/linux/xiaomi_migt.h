/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_XIAOMI_MIGT_H
#define _LINUX_XIAOMI_MIGT_H

#include <linux/types.h>

struct cpufreq_policy;
struct cpumask;
struct task_struct;

/* Native shared contracts recovered from the stock producers and callbacks.
 * Rebuild MIGT and its providers together; stock CFI identities are unknown. */
typedef int (*migt_select_cpu_fn)(struct task_struct *task);
typedef bool (*migt_skip_cpus_fn)(struct cpumask *mask);
typedef bool (*migt_task_cpu_fn)(struct task_struct *task, unsigned int cpu);
typedef bool (*migt_cpu_fn)(unsigned int cpu);
typedef void (*migt_mask_fn)(struct cpumask *mask);
typedef bool (*migt_affinity_fn)(void);
typedef bool (*migt_group_fn)(int pid);
typedef void (*migt_oem_freq_fn)(void *data, unsigned long util,
		unsigned long mapped_freq, unsigned long max, unsigned long *freq,
		struct cpufreq_policy *policy, bool *need_update);

void register_game_migt_select_cpu_hook(migt_select_cpu_fn hook);
void unregister_game_migt_select_cpu_hook(void);
void register_game_migt_skip_cpus_hook(migt_skip_cpus_fn hook);
void unregister_game_migt_skip_cpus_hook(void);
void register_game_migt_should_skip_this_cpu_rt_hook(migt_task_cpu_fn hook);
void unregister_game_migt_should_skip_this_cpu_rt_hook(void);
void register_game_migt_should_skip_migrate_hook(migt_task_cpu_fn hook);
void unregister_game_migt_should_skip_migrate_hook(void);
void register_game_migt_skip_sched_tick_migrate_hook(migt_task_cpu_fn hook);
void unregister_game_migt_skip_sched_tick_migrate_hook(void);
void register_game_migt_should_skip_newidle_balance_hook(migt_cpu_fn hook);
void unregister_game_migt_should_skip_newidle_balance_hook(void);
void register_game_migt_skip_lowest_rt_hook(migt_mask_fn hook);
void unregister_game_migt_skip_lowest_rt_hook(void);
void register_game_migt_skip_vip_cpus_hook(migt_mask_fn hook);
void unregister_game_migt_skip_vip_cpus_hook(void);
void register_game_migt_should_skip_affinity_task_hook(migt_affinity_fn hook);
void unregister_game_migt_should_skip_affinity_task_hook(void);
void register_game_migt_group_dep_hook(migt_group_fn hook);
void unregister_game_migt_group_dep_hook(void);
void register_oem_freq_hook(migt_oem_freq_fn hook);
void unregister_oem_freq_hook(void);

#endif
