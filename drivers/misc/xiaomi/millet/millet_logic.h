/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 GitHub @YorokobiMaster */
#ifndef _XIAOMI_MILLET_LOGIC_H_
#define _XIAOMI_MILLET_LOGIC_H_

#include "millet.h"

static inline bool millet_valid_type(millet_u32 type)
{
	return type > MILLET_ID_NULL && type < MILLET_ID_INVALID;
}

static inline bool millet_valid_kind(millet_u32 kind)
{
	return kind >= MILLET_MSG_ACK && kind <= MILLET_MSG_KIND_MAX;
}

static inline millet_u32 millet_route_for_type(millet_u32 type)
{
	if (type == MILLET_ID_HANDSHAKE)
		return MILLET_ID_SIG;
	if (type == MILLET_ID_BINDER_STAT)
		return MILLET_ID_BINDER;
	return type;
}

static inline bool millet_pkg_uid_reportable(millet_u32 uid)
{
	return uid >= MILLET_APP_UID_MIN;
}

static inline bool millet_down_valid(const struct millet_down_msg *msg,
				     size_t len)
{
	return msg && len >= sizeof(*msg) && millet_valid_type(msg->type) &&
		(msg->command == MILLET_CMD_HANDSHAKE ||
		 msg->command == MILLET_CMD_DISPATCH) &&
		msg->source == MILLET_DOWN_SOURCE &&
		msg->destination == MILLET_DOWN_DESTINATION;
}

static inline bool millet_signal_selected(unsigned int sig)
{
	return sig == 3 || sig == 6 || sig == 9 || sig == 15;
}

static inline void millet_prepare_event(struct millet_up_msg *msg,
					millet_u32 type)
{
	msg->type = type;
	msg->kind = MILLET_MSG_EVENT;
}

static inline void millet_prepare_sig_event(struct millet_up_msg *msg,
					    millet_u32 uid,
					    millet_u32 pid)
{
	millet_prepare_event(msg, MILLET_ID_SIG);
	msg->uid = uid;
	msg->payload.sig.pid = pid;
}

static inline void millet_prepare_pkg_event(struct millet_up_msg *msg)
{
	millet_prepare_event(msg, MILLET_ID_PKG);
	msg->uid = (millet_u32)msg->payload.pkg.uid;
}

/* Returns 1 when inserted, 0 for an existing/zero UID, and -1 when full. */
static inline int millet_uid_add(millet_u32 *uids, size_t count, millet_u32 uid)
{
	size_t i;
	size_t free_slot = count;

	if (!uid)
		return 0;

	for (i = 0; i < count; i++) {
		if (uids[i] == uid)
			return 0;
		if (!uids[i] && free_slot == count)
			free_slot = i;
	}

	if (free_slot == count)
		return -1;

	uids[free_slot] = uid;
	return 1;
}

static inline bool millet_uid_remove(millet_u32 *uids, size_t count,
				     millet_u32 uid)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (uids[i] == uid) {
			uids[i] = 0;
			return true;
		}
	}
	return false;
}

static inline void millet_uid_clear(millet_u32 *uids, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		uids[i] = 0;
}

static inline bool millet_uid_consume(millet_u32 *uids, size_t count,
				      millet_u32 uid)
{
	return millet_uid_remove(uids, count, uid);
}

#endif /* _XIAOMI_MILLET_LOGIC_H_ */
