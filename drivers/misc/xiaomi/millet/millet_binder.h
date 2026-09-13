/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 GitHub @YorokobiMaster */
#ifndef _XIAOMI_MILLET_BINDER_H_
#define _XIAOMI_MILLET_BINDER_H_

#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>

struct task_struct;

typedef void (*millet_binder_event_cb)(struct task_struct *primary,
				       struct task_struct *peer, int pid,
				       bool one_way, u32 code);

/* Stock's seven-word registration ABI, recovered from dash-millet-binder.md. */
struct millet_binder_hooks {
	unsigned long async_space_multiplier;
	unsigned long warn_ahead_space;
	millet_binder_event_cb overflow;
	millet_binder_event_cb reply;
	millet_binder_event_cb transaction;
	millet_binder_event_cb wait;
	void (*query_stat)(u32 uid, struct task_struct *requester, u32 status,
			   int requester_pid, bool active);
};

static_assert(sizeof(struct millet_binder_hooks) == 7 * sizeof(unsigned long));
static_assert(offsetof(struct millet_binder_hooks, overflow) == 2 * sizeof(unsigned long));
static_assert(offsetof(struct millet_binder_hooks, query_stat) == 6 * sizeof(unsigned long));

int oem_register_binder_hook(const struct millet_binder_hooks *hooks);
void oem_unregister_binder_hook(const struct millet_binder_hooks *hooks);
void query_binder_app_stat(u32 uid);

#endif /* _XIAOMI_MILLET_BINDER_H_ */
