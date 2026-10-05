/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_COLDSTART_H
#define _XIAOMI_METIS_COLDSTART_H
#include "metis_link.h"
struct kernel_param;
struct rw_semaphore;

/* Real controller/wakeup-owned globals: definitions, defaults and lifetime
 * are deliberately absent until their stock owners are reconstructed. */
extern u32 set_cold_start_render_vip;
extern u32 override_schedboost_in_coldstart;
extern u32 coldstart_set_mmlock_holder_vip;
extern u32 stask_prefer_cluster;
extern u32 cold_start_ui_cpu;
extern s32 cold_start_ui_state;

/* .text+0xd0: x1 mask, x2 duration added to jiffies; no fake policy helper. */
extern void set_mi_vip_task_common_noresched(struct task_struct *task,
		unsigned long mask, unsigned long duration_jiffies);

bool coldstart_app_ren_task(struct task_struct *task);
bool coldstart_app_tasks(struct task_struct *task);
bool coldstart_app_ui_pid(int pid);
bool coldstart_app_ui_task(struct task_struct *task);
void coldstart_mark_mmlock_holder(struct task_struct *task);
bool coldstart_override_schedboost(struct task_struct *task);
bool coldstart_sserver_tasks(struct task_struct *task);
void coldstart_tsk_ready_to_run(struct task_struct *task);
void coldstart_unmark_mmlock_holder(struct task_struct *task);
int get_coldstart_app_tgid(void);
int get_protect_dur_us(void);
int get_sserver_tgid(char *buffer, const struct kernel_param *param);
bool is_important_mmlock(struct rw_semaphore *mmlock);
void protect_cs_task(struct task_struct *task);
void protect_pre_cs_task(struct task_struct *task);
int set_cold_start_app_tgid(int tid);
int set_sserver_tgid(const char *buffer, const struct kernel_param *param);
void save_home_tgid(int tgid);
void save_systemui_tgid(int tgid);
void save_systemserver_tgid(int tgid);
void save_sf_tgid(int tgid);

/* These are consumers/classifiers only; no wakeup producer or hook here. */
bool cold_start_competing_with_ui(int cpu);
bool cold_start_ind_task(struct task_struct *task);
bool cold_start_link_task(struct task_struct *task);
bool cold_start_ui_pipeline(struct task_struct *task);
int cold_start_ui_recent_used_cpu(void);
int cold_start_ui_sched_state(void);
int cold_start_ui_sleep(void);
bool cold_start_ui_task(struct task_struct *task);
bool cold_start_wakeup_link_task(struct task_struct *task);
bool cold_start_wakeup_mark(struct task_struct *task);
bool cold_start_wakeup_task(struct task_struct *task);
bool cold_start_wakeup_task_common(struct task_struct *task,
		unsigned long mask);
#endif
