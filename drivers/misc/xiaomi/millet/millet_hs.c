// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 GitHub @YorokobiMaster */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>

#include "millet.h"
#include "millet_logic.h"

static void millet_hs_recv(struct millet_down_msg *down, unsigned int len)
{
	struct millet_up_msg up = { };

	if (!down || len < sizeof(*down))
		return;
	millet_prepare_event(&up, MILLET_ID_HANDSHAKE);
	millet_sendmsg(MILLET_ID_HANDSHAKE, current, &up);
}

static int millet_hs_send(struct task_struct *task, struct millet_up_msg *msg,
			  struct millet_sock *sock)
{
	if (!task || !msg || !sock)
		return -EINVAL;
	millet_prepare_event(msg, MILLET_ID_HANDSHAKE);
	return millet_sendto_user(task, msg, sock);
}

static void millet_hs_subsystem_init(struct millet_sock *sock)
{
	/*
	 * Intentional stock difference (dash-millet.md, M004): stock writes
	 * core-private route/enabled fields here. Core now owns that state.
	 */
	(void)sock;
}

static int __init millet_hs_init(void)
{
	int ret;

	ret = register_millet_hook(MILLET_ID_HANDSHAKE, millet_hs_recv,
				   millet_hs_send, millet_hs_subsystem_init);
	if (ret)
		return ret;
	ret = init_millet_subsystem(MILLET_ID_HANDSHAKE);
	if (ret)
		unregister_millet_hook(MILLET_ID_HANDSHAKE);
	return ret;
}

static void __exit millet_hs_exit(void)
{
	unregister_millet_hook(MILLET_ID_HANDSHAKE);
}

module_init(millet_hs_init);
module_exit(millet_hs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GitHub @YorokobiMaster");
MODULE_DESCRIPTION("Xiaomi Millet handshake subsystem");
