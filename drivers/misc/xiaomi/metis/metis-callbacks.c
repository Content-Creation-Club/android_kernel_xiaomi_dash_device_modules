// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL stock callback slice. Not a replacement metis module. */
#include <linux/kernel.h>
#include "metis_callbacks.h"

/* Signed acceptance precedes the unsigned bounds trap in stock. Keep both:
 * IDs 3..INT_MAX log/return, whereas bit-31 IDs log then BRK 0x5512. */
static bool callback_module(unsigned int module, const char *function,
			    bool comma)
{
	if ((int)module >= 3) {
		pr_err("metis-fas: invalid mod register\n");
		return false;
	}
	if (comma)
		pr_info("metis-fas: %d, %s now\n", module, function);
	else
		pr_info("metis-fas: %d %s now\n", module, function);
	if (module > 2) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	return true;
}

void register_render_task_detect(unsigned int module, metis_detect_fn detect,
				metis_detect_fn pipeline)
{
	if (!callback_module(module, __func__, true))
		return;
	render_task_detect[module].detect = detect;
	/* Stock's second pointer store retains its distinct BRK #1 check. */
	if ((((unsigned long)module << 4) | 8) > 0x30) {
		asm volatile("brk #1");
		__builtin_unreachable();
	}
	render_task_detect[module].pipeline = pipeline;
}

void unregister_render_task_detect(unsigned int module)
{
	if (!callback_module(module, __func__, true))
		return;
	render_task_detect[module].detect = NULL;
	render_task_detect[module].pipeline = NULL;
}

void register_ui_task_detect(unsigned int module, metis_detect_fn detect,
				metis_detect_fn pipeline)
{
	if (!callback_module(module, __func__, true))
		return;
	ui_task_detect[module].detect = detect;
	/* Stock's second pointer store retains its distinct BRK #1 check. */
	if ((((unsigned long)module << 4) | 8) > 0x30) {
		asm volatile("brk #1");
		__builtin_unreachable();
	}
	ui_task_detect[module].pipeline = pipeline;
}

void unregister_ui_task_detect(unsigned int module)
{
	if (!callback_module(module, __func__, true))
		return;
	ui_task_detect[module].detect = NULL;
	ui_task_detect[module].pipeline = NULL;
}

void register_override_schedboost(unsigned int module, metis_detect_fn detect)
{
	if (callback_module(module, __func__, false))
		override_schedboost[module] = detect;
}

void unregister_override_schedboost(unsigned int module)
{
	if (callback_module(module, __func__, false))
		override_schedboost[module] = NULL;
}

bool mi_render_task(struct task_struct *task)
{
	bool result = false;
	unsigned int module;

	/* Call all populated slots, even after a true result. No snapshot/RCU. */
	for (module = 0; module < 3; module++) {
		if (render_task_detect[module].detect)
			result |= render_task_detect[module].detect(task);
	}
	return result;
}

bool mi_render_pipeline(struct task_struct *task)
{
	bool result = false;
	unsigned int module;

	/* Call all populated slots, even after a true result. No snapshot/RCU. */
	for (module = 0; module < 3; module++) {
		if (render_task_detect[module].pipeline)
			result |= render_task_detect[module].pipeline(task);
	}
	return result;
}

bool mi_ui_task(struct task_struct *task)
{
	bool result = false;
	unsigned int module;

	/* Call all populated slots, even after a true result. No snapshot/RCU. */
	for (module = 0; module < 3; module++) {
		if (ui_task_detect[module].detect)
			result |= ui_task_detect[module].detect(task);
	}
	return result;
}

bool mi_ui_pipeline(struct task_struct *task)
{
	bool result = false;
	unsigned int module;

	/* Call all populated slots, even after a true result. No snapshot/RCU. */
	for (module = 0; module < 3; module++) {
		if (ui_task_detect[module].pipeline)
			result |= ui_task_detect[module].pipeline(task);
	}
	return result;
}

bool sched_boost_should_disable(struct task_struct *task)
{
	bool result = false;
	unsigned int module;

	for (module = 0; module < 3; module++) {
		if (override_schedboost[module])
			result |= override_schedboost[module](task);
	}
	return result;
}
