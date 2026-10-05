/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_COMMON_H
#define _XIAOMI_METIS_COMMON_H
#include <linux/atomic.h>
#include "metis_classify.h"
#include "metis_link.h"
struct kernel_param;

/* Stock-owned objects. Full state initialization/control remains absent. */
extern s32 link_debug, mi_link_enable, mi_async_binder_link_enable;
extern s32 mi_fboost_enable, preempt_sysprocess;
extern s32 speed_touch_enable, trace_motion_event;
extern s64 touch_up_start;
extern unsigned long mi_link_boost_jif;
extern atomic_t mi_dynamic_vip_num, mi_sdk_eboost_vip_num;
extern u32 perf_min_freq[3];
typedef int (*metis_sched_enable_fn)(void);
extern metis_sched_enable_fn mi_sched_enable[3];
/* Wakeup-owned state; its initialization/producers remain absent. */
extern s32 set_possible_notifythr_vip, last_ui_forked_pid;
extern unsigned long last_ui_forked_jif, cold_start_ui_futex_wait;
bool task_maybe_notify_thread(struct task_struct *task);
void hook_dynamic_vip_setting(struct task_struct *task, bool set);

void mi_resched_vip_task(struct task_struct *task);
void set_mi_vip_task_common(struct task_struct *task, unsigned long mask,
			  unsigned long duration);
void set_mi_vip_task_common_noresched(struct task_struct *task,
				    unsigned long mask, unsigned long duration);
void clean_mi_vip_task_common(struct task_struct *task, unsigned long mask);
bool mi_lock_vip_task(struct task_struct *task);
bool mi_binder_vip_task(struct task_struct *task);
bool mi_art_mutex_vip_task(struct task_struct *task);
void set_mi_vip_task(struct task_struct *task, u32 duration, unsigned long mask);
s32 mi_task_uid(struct mi_task_struct *state);
void set_mi_dynamic_vip_task(struct task_struct *task, u32 duration);
void clean_mi_dynamic_vip_task(struct task_struct *task);
bool mi_dynamic_vip_task(struct task_struct *task);
void mi_dynamic_vtask_prio_sched_start(struct task_struct *task, int milliseconds);
void mi_dynamic_vtask_prio_sched_end(struct task_struct *task);
void mi_binder_restore_prio_sched_start(struct task_struct *task, int milliseconds);
void metis_clean_vip_thread(struct task_struct *task);
bool metis_xxx_task(struct task_struct *task, unsigned long mask);
bool metis_vip_task(struct task_struct *task);
bool metis_ren_task(struct task_struct *task);
bool metis_sup_task(struct task_struct *task);
bool metis_ip_task(struct task_struct *task);
bool metis_vvip_task(struct task_struct *task);
bool metis_ui_task_quick(struct task_struct *task);
bool metis_ui_pipeline(struct task_struct *task);
void metis_set_ui_task(struct task_struct *task);
u32 get_preempt_prio(struct task_struct *task);
s32 get_metis_deadline_ms(struct mi_task_struct *state);
u8 *get_important_task_info(struct mi_task_struct *state);
u32 get_process_config_index(struct task_struct *task);
bool is_uuser_set_vip(struct task_struct *task);
bool is_uuser_set_ip(struct task_struct *task);

int fas_suspend(void);
void fas_resume(void);
bool metis_sched_enable(void);
void register_mi_sched_enable(unsigned int module, metis_sched_enable_fn enable);
void unregister_mi_sched_enable(unsigned int module);
int get_link_vip_enable(char *buffer, const struct kernel_param *param);
int set_link_vip_enable(const char *value, const struct kernel_param *param);
int get_async_binder_link_vip_enable(char *buffer, const struct kernel_param *param);
int set_async_binder_link_vip_enable(const char *value,
				     const struct kernel_param *param);
int get_fboost_enable(char *buffer, const struct kernel_param *param);
int set_fboost_enable(const char *value, const struct kernel_param *param);
int get_mi_fboost_enable(void);
bool get_mi_link_boost_enable(void);
int metis_preempt_sysprocess(struct task_struct *task);
bool in_interest_input_event(void);
#endif
