/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_MONITOR_H
#define _XIAOMI_METIS_MONITOR_H
#include "metis_extra_controls.h"
#include "metis_lowlt.h"

/* Driver-owned record: .bss+0x15bd0, size0x1e0 (12 x 0x28). */
struct metis_run_history {
	s32 pid;
	char comm[16];
	u32 unknown_14;
	u64 running_time;
	u64 sampled_at;
};
extern struct metis_run_history his_lru_task[12];
extern u32 sched_doctor_enable;
extern s32 normal_running_too_long_thresh;
extern u32 suspend_vip_enable;
extern unsigned int doublecyc_debug;

void monitor_task_enqueue(struct mi_task_struct *state);
void mi_scheduler_tick_hook(struct rq *rq);
#endif
