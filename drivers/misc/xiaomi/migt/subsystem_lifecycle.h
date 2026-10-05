/* SPDX-License-Identifier: GPL-2.0 */
/* Recovered helper contracts; module entry/state definitions remain absent. */
#ifndef XIAOMI_MIGT_SUBSYSTEM_LIFECYCLE_H
#define XIAOMI_MIGT_SUBSYSTEM_LIFECYCLE_H
#include <linux/irq_work.h>
#include "worker.h"
#include "../metis/metis_registry.h"

/* Actual stock local state/table owners; no fabricated definitions. */
extern struct ctl_table_header *migt_sched_header, *game_load_header;
extern struct ctl_table migt_table[7], game_load_table[11];
extern struct proc_dir_entry *frame_load_dir;
/* Distinct from game-load's same-named original static proc entries. */
extern struct proc_dir_entry *migt_frame_history_entry, *migt_frame_cpu_entry;
extern u32 flw_work_in_process;
extern struct irq_work flw_cpufreq_irq_work;

/* Canonical private callback types inferred from real producer/body evidence.
 * Native typedef/KCFI acceptance remains untested; providers are not replaced. */
typedef int (*migt_select_cpu_fn)(struct task_struct *task);
typedef bool (*migt_task_cpu_fn)(struct task_struct *task, unsigned int cpu);
typedef bool (*migt_cpu_fn)(unsigned int cpu);
typedef void (*migt_mask_fn)(struct cpumask *mask);
typedef bool (*migt_affinity_fn)(void);
typedef bool (*migt_group_fn)(int pid);
typedef void (*migt_wake_fn)(struct task_struct *task, unsigned int wake_flags);
typedef void (*migt_oem_freq_fn)(void *data, unsigned long util,
		unsigned long mapped_freq, unsigned long max, unsigned long *freq,
		struct cpufreq_policy *policy, bool *need_update);

void register_game_migt_select_cpu_hook(migt_select_cpu_fn hook);
void unregister_game_migt_select_cpu_hook(void);
void register_game_migt_skip_cpus_hook(metis_skip_cpus_fn hook);
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
void register_game_wake_f(migt_wake_fn hook);
void unregister_game_wake_f(void);
void register_oem_freq_hook(migt_oem_freq_fn hook);
void unregister_oem_freq_hook(void);
void android_rvh_cpufreq_transition(void *data, struct cpufreq_policy *policy);

int migt_sched_init(void);
void migt_sched_exit(void);
int frame_load_init(struct proc_dir_entry *parent);
void frame_load_exit(void);
int game_load_init(void);
void game_load_exit(void);
void game_link_init(void);
void game_link_exit(void);
#endif
