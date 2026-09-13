// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 GitHub @YorokobiMaster */

#include <linux/capability.h>
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uidgid.h>

#include "millet_oem_cgroup_policy.h"

static int millet_policy_can_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *dst_css;
	struct task_struct *task;
	bool system = uid_eq(current_euid(), KUIDT_INIT(1000));

	cgroup_taskset_for_each(task, dst_css, tset) {
		bool self = task == current;
		bool admin = false;

		if (!self && !system)
			admin = capable(CAP_SYS_ADMIN);
		if (millet_oem_cgroup_policy_allows(self, system, admin))
			continue;

		pr_warn_ratelimited("millet_oem_cgroup: denied cross-task migration\n");
		/*
		 * Stock returns +1 after logging every denial. Return -EPERM so the
		 * cgroup write reports failure instead of a successful one-byte write.
		 * Evidence: C004 in dash-millet-oem-cgroup.md.
		 */
		return -EPERM;
	}

	return 0;
}

static int __init millet_oem_cgroup_init(void)
{
	bool can_attach;
	bool cancel_attach;

	/*
	 * Stock overwrites both callbacks unlocked. Refuse an occupied slot: an
	 * arbitrary callback cannot be chained safely without owning its module
	 * lifetime and can_attach/cancel_attach accounting. This policy keeps no
	 * attach state, so it needs no cancel callback. Evidence: C005 in
	 * dash-millet-oem-cgroup.md.
	 *
	 * All callback consumers run from cgroup migration with cgroup_mutex held.
	 * Taking the same exported lock serializes the check and publication with
	 * those consumers. Evidence: C006 in dash-millet-oem-cgroup.md.
	 */
	cgroup_lock();
	can_attach = !!freezer_cgrp_subsys.can_attach;
	cancel_attach = !!freezer_cgrp_subsys.cancel_attach;
	if (!millet_oem_cgroup_can_install(can_attach, cancel_attach)) {
		cgroup_unlock();
		pr_err("millet_oem_cgroup: freezer callbacks already installed\n");
		return -EBUSY;
	}
	freezer_cgrp_subsys.can_attach = millet_policy_can_attach;
	cgroup_unlock();

	return 0;
}
module_init(millet_oem_cgroup_init);

/*
 * Stock has no exit and leaves callbacks pointing into module text. Keep the
 * reconstruction non-unloadable rather than add an unproven teardown path.
 * Evidence: C007 in dash-millet-oem-cgroup.md.
 */

MODULE_AUTHOR("GitHub @YorokobiMaster");
MODULE_DESCRIPTION("Xiaomi Millet legacy freezer cgroup policy");
MODULE_LICENSE("GPL");
