/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_LINK_H
#define _XIAOMI_METIS_LINK_H
#include "metis_classify.h"
extern s32 metis_wakeup_enable;
/* Real policy cleaner, .text+0x940..0xb44, now in metis-common.c.
 * Whole-module state/lifecycle and registration remain incomplete. */
extern void clean_mi_vip_task_common(struct task_struct *task, unsigned long mask);
bool in_coldstart_period(void);
bool in_cold_start_wakeup(void);
bool mi_link_vip_task(struct task_struct *task);
#endif
