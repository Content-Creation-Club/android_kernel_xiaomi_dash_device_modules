/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_WAKEUP_H
#define _XIAOMI_METIS_WAKEUP_H
#include "metis_coldstart.h"
#include "metis_common.h"
#include "metis_extra_controls.h"
struct rq;
struct cpumask;
struct kernel_param;

extern u32 metis_wakeup_debug;
/* .bss+0x3bea4, 0xf0 bytes. Initialization/control ownership is absent. */
extern char app_wakeup_task[240];
int metis_wakeup_ui_pipeline_select_rq(struct task_struct *task,
		struct cpumask *excluded, int wake_flags);
bool metis_wakeup_futex_plist_insert(int *priority);
void metis_wakeup_cold_start_skip_ui_cpumask(struct task_struct *task,
		struct cpumask *mask);
bool metis_wakeup_cold_start_skip_ui_cpu(struct task_struct *task, int cpu);
void metis_wakeup_enqueue_task_hook(struct rq *rq, struct task_struct *task);
void metis_wakeup_dequeue_task_hook(void *unused, struct rq *rq,
		struct task_struct *task, int flags);
void set_coldstart_wakeup_task(struct task_struct *task);
void wake_up_new_task_hook(void *unused, struct task_struct *task);
void metis_wakeup_try_to_wakeup(struct task_struct *task);
void metis_wakeup_clear_wakeup_task(struct task_struct *task, int enabled);
void metis_wakeup_syscall_prctl_finished_hook(void *unused, int option,
		struct task_struct *task);
void metis_wakeup_futex_wait_hook(void *unused, int cmd, unsigned int *flags,
		u32 __user *uaddr2);
int set_metis_wakeup_task(const char *value, const struct kernel_param *param);
int metis_wakeup_init(void);
void metis_wakeup_exit(void);
#endif
