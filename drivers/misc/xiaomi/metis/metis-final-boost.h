/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_FINAL_BOOST_H
#define _XIAOMI_METIS_FINAL_BOOST_H

#include <linux/percpu.h>
#include <linux/pm_qos.h>
#include "metis_cluster_boost.h"
#include "metis_extra_controls.h"

/* Genuine .data..percpu requests, originally at 0x0 and 0x40. */
DECLARE_PER_CPU(struct freq_qos_request, metis_req_min);
DECLARE_PER_CPU(struct freq_qos_request, metis_req_max);
extern struct workqueue_struct *metis_wq;
/* Original local cpus_visit.0, eight bytes; notifier truncates it to u32. */
extern unsigned long cpus_visit_0;
extern u32 min_cluster_freqs[3];

/* Native kthread and notifier callback contracts recover unused formals. */
int fboost_run(void *data);
int metis_fboost_init(void);
void metis_fboost_exit(void);
int metis_freq_qos_request_init(struct notifier_block *notifier,
			      unsigned long event, void *data);
void register_corectl_boost_hook(metis_corectl_boost_fn hook);
void register_get_perflock_last_freqs_hook(metis_perflock_last_freqs_fn hook);
void register_update_perflock_freq_hook(metis_update_perflock_freq_fn hook);

#endif
