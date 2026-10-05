/* SPDX-License-Identifier: GPL-2.0 */
/* Source-only worker and scheduler contracts; no startup or registration. */
#ifndef XIAOMI_MIGT_WORKER_H
#define XIAOMI_MIGT_WORKER_H
#include "user_api.h"

/* Real omitted module/controller owners, not replacement defaults. */
extern u32 force_cluster_sched_enable, force_forbidden_walt_lb;
extern u32 task_enqueue_time_toolong, set_render_as_stask;
extern u32 stask_candidate_num, vip_task_max_num, ip_task_max_num;
extern u32 need_reset_runtime_everytime, pick_stask_policy;
/* Stock .bss+0x548, 1024 x 16 bytes. unknown_00 holds the GT alias. */
extern struct migt_rank_entry user_array[1024];
/* Real metis/provider imports, still requiring their lifecycle owners. */
extern int ui_run_cpu, render_run_cpu;
struct rq;
bool rq_deadloop_detect(struct rq *rq);

void find_energy_hook(struct task_struct *task, bool *is_vip);
void walt_get_indicies_hook(struct task_struct *task, u32 *cluster,
		int *start_index, int unused_reason, bool *handled);
bool game_lb_control(struct task_struct *task, int source_cpu,
		int destination_cpu, int unused_capacity_delta);
/* Distinct stock-local names in two translation units, same machine ABI. */
void migt_render_link_flw(struct task_struct *task, unsigned int wake_flags);
void migt_frame_link_flw(struct task_struct *task, unsigned int wake_flags);
int migt_hardwork(void *uid_work);
#endif
