/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 GitHub @YorokobiMaster */
#ifndef _XIAOMI_PERF_ACTUATOR_H_
#define _XIAOMI_PERF_ACTUATOR_H_

/*
 * Intentional source-only stock difference: the callback calling convention is
 * recovered, but Xiaomi's typedef spelling is unavailable. This name changes
 * the CONFIG_MODVERSIONS CRC and therefore cannot serve stock xr-qi.ko.
 */
typedef int (*perf_actuator_callback_t)(unsigned long arg);

int register_perf_actuator(unsigned int cmd,
			   perf_actuator_callback_t callback);
int unregister_perf_actuator(unsigned int cmd);

#endif /* _XIAOMI_PERF_ACTUATOR_H_ */
