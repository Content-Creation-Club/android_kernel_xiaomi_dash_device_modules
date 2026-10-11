// SPDX-License-Identifier: GPL-2.0-only
/* Observed controller consumers. Full parameter table/lifecycle absent. */
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/ktime.h>
#include "metis_common.h"

int fas_suspend(void)
{
	/* Stock saves W0 of ktime_get, then publishes the one-byte flag. */
	fas_ktime_last = (s32)ktime_to_ns(ktime_get());
	fas_ktime_suspended = 1;
	return 0;
}

void fas_resume(void)
{
	fas_ktime_suspended = 0;
}

bool metis_sched_enable(void)
{
	unsigned int result = 0, module;

	/* All populated slots run; OR full integers, then test nonzero. */
	for (module = 0; module < 3; module++) {
		if (mi_sched_enable[module])
			result |= mi_sched_enable[module]();
	}
	return result != 0;
}

void register_mi_sched_enable(unsigned int module, metis_sched_enable_fn enable)
{
	if ((int)module >= 3) {
		pr_err("metis-fas: invalid mod register\n");
		return;
	}
	pr_info("metis-fas: %d %s now\n", module, __func__);
	if (module >= 3) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	mi_sched_enable[module] = enable;
}

void unregister_mi_sched_enable(unsigned int module)
{
	if ((int)module >= 3) {
		pr_err("metis-fas: invalid mod register\n");
		return;
	}
	pr_info("metis-fas: %d %s now\n", module, __func__);
	if (module >= 3) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	mi_sched_enable[module] = NULL;
}

int get_link_vip_enable(char *buffer, const struct kernel_param *param)
{
	/* Stock ignores the output buffer and returns the knob as a byte count. */
	return mi_link_enable;
}

int set_link_vip_enable(const char *value, const struct kernel_param *param)
{
	u32 enabled = 0;

	if (sscanf(value, "%u\n", &enabled) != 1)
		return -EINVAL;
	mi_link_enable = enabled;
	return 0;
}

int get_async_binder_link_vip_enable(char *buffer, const struct kernel_param *param)
{
	return mi_async_binder_link_enable;
}

int set_async_binder_link_vip_enable(const char *value,
				     const struct kernel_param *param)
{
	u32 enabled = 0;

	if (sscanf(value, "%u\n", &enabled) != 1)
		return -EINVAL;
	mi_async_binder_link_enable = enabled;
	return 0;
}

int get_fboost_enable(char *buffer, const struct kernel_param *param)
{
	return mi_fboost_enable;
}

int set_fboost_enable(const char *value, const struct kernel_param *param)
{
	u32 enabled = 0;

	if (sscanf(value, "%u\n", &enabled) != 1)
		return -EINVAL;
	mi_fboost_enable = enabled;
	return 0;
}

int get_mi_fboost_enable(void)
{
	return mi_fboost_enable;
}

bool get_mi_link_boost_enable(void)
{
	return mi_fboost_enable &&
		((s64)((u64)jiffies - mi_link_boost_jif) < 0 ||
		 atomic_read(&mi_sdk_eboost_vip_num) > 0);
}

int metis_preempt_sysprocess(struct task_struct *task)
{
	return task ? preempt_sysprocess : 0;
}

bool in_interest_input_event(void)
{
	s64 deadline;

	if (!speed_touch_enable)
		return false;
	if (trace_motion_event == 1)
		return true;
	if (trace_motion_event != 2)
		return false;
	deadline = (s64)((u64)touch_up_start + 150000ULL);
	return ktime_to_ns(ktime_get()) / 1000 < deadline;
}
