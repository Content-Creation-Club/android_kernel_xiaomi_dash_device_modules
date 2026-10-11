/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_FINAL_OWNERS_H
#define _XIAOMI_METIS_FINAL_OWNERS_H

#include <linux/xiaomi_metis.h>
#include "metis-final-control.h"
#include "metis-final-link.h"
#include "metis-final-boost.h"
#include "metis-final-module.h"
#include "metis_registry.h"
#include "metis_io.h"

/* Genuine original objects not previously declared by the recovered slices.
 * Their parameter ops are retained independently of inferred C signedness. */
extern u32 mi_lock_blocked_pid, in_perf_mod, fperiod;
extern u32 limit_bgtask_sched, force_forbidden_walt_lb;
extern u32 force_viptask_select_rq, task_enqueue_time_toolong;
extern u32 reader_biased_jif_for_vip, metis_io_debug;
extern unsigned int n_freq;
extern char version[64];

/* Stock .bss+0x15a40 is an initially empty callback slot. Its current
 * callable type comes from the paired native provider's shared header;
 * the original canonical typedef remains UNKNOWN. */

#endif
