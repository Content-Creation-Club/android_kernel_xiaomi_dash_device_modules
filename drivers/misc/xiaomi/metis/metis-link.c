// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL stock link classification and cold-start expiry consumers. */
#include <linux/jiffies.h>
#include <linux/sched.h>
#include "metis_link.h"

bool in_coldstart_period(void)
{
	/* No protect_cs or TGID gate occurs in this stock predicate. */
	if ((s64)(coldstart_end_jif - coldstart_begin_jif) < 0 &&
	    (s64)(jiffies - coldstart_begin_jif - 1000UL) < 0)
		return true;
	cold_start_app_tgid = 0;
	home_tgid = 0;
	systemui_tgid = 0;
	systemserver_tgid = 0;
	sf_tgid = 0;
	cold_start_app_mmlock = NULL;
	cold_start_sserver_tgid = 0;
	cold_start_sserver_mmlock = NULL;
	home_mmlock = NULL;
	systemui_mmlock = NULL;
	systemserver_mmlock = NULL;
	sf_mmlock = NULL;
	return false;
}

bool in_cold_start_wakeup(void)
{
	if (metis_wakeup_enable)
		return in_coldstart_period();
	return false;
}

bool mi_link_vip_task(struct task_struct *task)
{
	struct mi_task_struct *state;
	u32 mask;

	/* Stock performs cold-start/global-expiry work before its NULL check. */
	if (in_cold_start_wakeup()) {
		state = get_mi_task_struct(task);
		if (state && ((state->unknown_c8 & 0x1e0) ||
			      (state->flags_0xc0 & 0x200)))
			return true;
	}
	if (!task)
		return false;
	state = get_mi_task_struct(task);
	if (!state)
		return false;
	mask = state->flags_0xc0 & 0x0c0ae000;
	if ((s64)((u64)jiffies - state->deadline_0x290) < 0)
		return mask != 0;
	if (mask)
		clean_mi_vip_task_common(task, 0x0c0ae000UL);
	return false;
}
