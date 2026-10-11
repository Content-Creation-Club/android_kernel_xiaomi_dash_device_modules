/* SPDX-License-Identifier: GPL-2.0 */
/* Recovered helper contracts; native provider integration remains untested. */
#ifndef XIAOMI_MIGT_SUBSYSTEM_LIFECYCLE_H
#define XIAOMI_MIGT_SUBSYSTEM_LIFECYCLE_H
#include <linux/irq_work.h>
#include <linux/xiaomi_migt.h>
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
typedef void (*migt_wake_fn)(struct task_struct *task, unsigned int wake_flags);
void register_game_wake_f(migt_wake_fn hook);
void unregister_game_wake_f(void);
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
