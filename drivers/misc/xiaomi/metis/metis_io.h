/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_IO_H
#define _XIAOMI_METIS_IO_H
#include <linux/types.h>
struct task_struct;
struct work_struct;

/* Stock .data+0x401c/+0x4020 and .bss+0x3bfc0. Owners remain absent. */
extern u32 metis_io_enabled;
extern char VERITY_WORK_NAME[12];
extern u64 vip_iowait_time;

void metis_io_schedule_hook(struct task_struct *task);
void probe_workqueue_execute_start(void *unused, struct work_struct *work);
void metis_io_init(void);
void metis_io_exit(void);
#endif
