/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_BINDER_REQUEST_H
#define _XIAOMI_METIS_BINDER_REQUEST_H
#include "metis_state.h"

/* Real stock owner .bss+0x15a80, size8; its startup/controller is absent. */
extern unsigned long mi_link_boost_jif;
void set_mi_req_async_binder_link_task(struct task_struct *task, u32 ticks);
void clean_mi_req_async_binder_link_task(struct task_struct *task);
void mi_req_async_binder_link_sched_start(struct task_struct *task, u32 milliseconds);
void mi_req_async_binder_link_sched_end(struct task_struct *task);
#endif
