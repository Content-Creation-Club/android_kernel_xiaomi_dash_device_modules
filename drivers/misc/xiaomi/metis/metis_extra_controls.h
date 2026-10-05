/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_EXTRA_CONTROLS_H
#define _XIAOMI_METIS_EXTRA_CONTROLS_H
#include "metis_common.h"
struct rq;

/* Original state; full initialization/controller ownership remains absent. */
extern u32 mi_boost_duration;
extern u8 vtask_monitor;
extern s32 ui_run_cpu, render_run_cpu;

void clean_coldstart_data(void);
bool in_coldstart(void);
bool is_home_tasks(struct task_struct *task);
bool is_systemui_tasks(struct task_struct *task);
bool is_systemserver_tasks(struct task_struct *task);
bool is_sf_tasks(struct task_struct *task);
void clear_preemption_protect(struct task_struct *task);
void metis_set_ui_pipeline(struct task_struct *task);
void metis_clean_ui_pipeline(struct task_struct *task);
bool metis_render_pipeline(struct task_struct *task);
int set_prefer_cluster_with_check(const char *value,
				const struct kernel_param *param);
int set_speed_touch_enable(const char *value, const struct kernel_param *param);
int set_trace_motion_event(const char *value, const struct kernel_param *param);
void get_frame_boost_end(unsigned long *duration);
u8 get_vtask_monitor_status(unsigned long *begin_jiffies);
u32 get_thread_type(struct task_struct *task);
bool curr_mmaplock_need_prio(void);
void mi_set_rt_binder_free(struct task_struct *task);
void mi_restore_rt_binder_free(struct task_struct *task);
void mi_monitor_hook(struct rq *rq, struct task_struct *task);
void find_energy_hook(struct task_struct *task, bool *is_vip);
void mi_after_enqueue_task_hook(void *unused, struct rq *rq,
			       struct task_struct *task, int flags);
void mi_after_dequeue_task_hook(void *unused, struct rq *rq,
			       struct task_struct *task, int flags);
void mi_enqueue_task_fair_hook(struct rq *rq, struct task_struct *task);
#endif
