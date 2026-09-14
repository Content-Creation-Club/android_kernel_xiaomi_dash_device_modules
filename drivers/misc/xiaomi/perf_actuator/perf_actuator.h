/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 GitHub @YorokobiMaster */
#ifndef _XIAOMI_PERF_ACTUATOR_H_
#define _XIAOMI_PERF_ACTUATOR_H_

/*
 * Stock provider and xr-qi KCFI IDs identify this as int (void *).
 * Xiaomi's typedef spelling remains unavailable, so CONFIG_MODVERSIONS stock
 * compatibility must still be checked separately.
 */
typedef int (*perf_actuator_callback_t)(void *arg);

int register_perf_actuator(unsigned int cmd,
			   perf_actuator_callback_t callback);
int unregister_perf_actuator(unsigned int cmd);

#endif /* _XIAOMI_PERF_ACTUATOR_H_ */
