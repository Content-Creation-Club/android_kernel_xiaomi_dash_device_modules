/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 GitHub @YorokobiMaster */

#ifndef _MILLET_OEM_CGROUP_POLICY_H_
#define _MILLET_OEM_CGROUP_POLICY_H_

static inline int millet_oem_cgroup_policy_allows(int self, int system,
						  int admin)
{
	return self || system || admin;
}

static inline int millet_oem_cgroup_can_install(int can_attach,
						int cancel_attach)
{
	return !can_attach && !cancel_attach;
}

#endif
