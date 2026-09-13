/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 GitHub @YorokobiMaster */
#ifndef _XIAOMI_MILLET_H_
#define _XIAOMI_MILLET_H_

#ifdef __KERNEL__
#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>
typedef __u8 millet_u8;
typedef __u32 millet_u32;
typedef __u64 millet_u64;
typedef __s64 millet_s64;
#define MILLET_STATIC_ASSERT(expr) static_assert(expr)
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t millet_u8;
typedef uint32_t millet_u32;
typedef uint64_t millet_u64;
typedef int64_t millet_s64;
#define MILLET_STATIC_ASSERT(expr) _Static_assert((expr), #expr)
#endif

#define MILLET_NETLINK_PROTOCOL 30
#define MILLET_DOWN_SOURCE 0x00000000abcddcbaULL
#define MILLET_DOWN_DESTINATION 0x0000000012341234ULL
#define MILLET_UP_SOURCE 0x0000000012341234ULL
#define MILLET_UP_DESTINATION 0x00000000abcddcbaULL
#define MILLET_APP_UID_MIN 10000U
#define MILLET_UID_SLOTS 64U

enum millet_subsystem_id {
	MILLET_ID_NULL = 0,
	MILLET_ID_SIG = 1,
	MILLET_ID_BINDER = 2,
	MILLET_ID_BINDER_STAT = 3,
	MILLET_ID_MEM = 4,
	MILLET_ID_PKG = 5,
	MILLET_ID_HANDSHAKE = 6,
	MILLET_ID_INVALID = 7,
};

enum millet_command {
	MILLET_CMD_HANDSHAKE = 1,
	MILLET_CMD_DISPATCH = 3,
};

enum millet_message_kind {
	MILLET_MSG_ACK = 1,
	MILLET_MSG_EVENT = 2,
};

/* Stock send validation accepts kind 3; no supplied satellite names its semantics. */
#define MILLET_MSG_KIND_MAX 3U

enum millet_pkg_operation {
	MILLET_PKG_ADD = 1,
	MILLET_PKG_REMOVE = 2,
	MILLET_PKG_CLEAR = 3,
};

/* Dash stock uses native little-endian integer fields, without byte swapping. */
struct millet_down_msg {
	millet_u32 type;
	millet_u32 command;
	millet_u64 source;
	millet_u64 destination;
	union {
		millet_u8 raw[56];
		struct {
			millet_u8 reserved[48];
			millet_u32 operation;
			millet_u32 uid;
		} pkg;
	} payload;
};

struct millet_up_msg {
	millet_u32 type;
	millet_u32 route;
	millet_u32 kind;
	millet_u32 reserved1;
	millet_u64 source;
	millet_u64 destination;
	millet_u32 uid;
	millet_u32 reserved2;
	millet_s64 timestamp_sec;
	millet_s64 timestamp_nsec;
	union {
		millet_u8 raw[88];
		struct {
			millet_u8 reserved[64];
			millet_u32 pid;
			millet_u8 tail[20];
		} sig;
		struct {
			millet_u8 reserved[48];
			millet_u64 uid;
			millet_u8 tail[32];
		} pkg;
	} payload;
};

MILLET_STATIC_ASSERT(sizeof(struct millet_down_msg) == 80);
MILLET_STATIC_ASSERT(offsetof(struct millet_down_msg, type) == 0);
MILLET_STATIC_ASSERT(offsetof(struct millet_down_msg, command) == 4);
MILLET_STATIC_ASSERT(offsetof(struct millet_down_msg, source) == 8);
MILLET_STATIC_ASSERT(offsetof(struct millet_down_msg, destination) == 16);
MILLET_STATIC_ASSERT(offsetof(struct millet_down_msg, payload.pkg.operation) == 72);
MILLET_STATIC_ASSERT(offsetof(struct millet_down_msg, payload.pkg.uid) == 76);
MILLET_STATIC_ASSERT(sizeof(struct millet_up_msg) == 144);
MILLET_STATIC_ASSERT(offsetof(struct millet_up_msg, route) == 4);
MILLET_STATIC_ASSERT(offsetof(struct millet_up_msg, kind) == 8);
MILLET_STATIC_ASSERT(offsetof(struct millet_up_msg, source) == 16);
MILLET_STATIC_ASSERT(offsetof(struct millet_up_msg, destination) == 24);
MILLET_STATIC_ASSERT(offsetof(struct millet_up_msg, uid) == 32);
MILLET_STATIC_ASSERT(offsetof(struct millet_up_msg, timestamp_sec) == 40);
MILLET_STATIC_ASSERT(offsetof(struct millet_up_msg, timestamp_nsec) == 48);
MILLET_STATIC_ASSERT(offsetof(struct millet_up_msg, payload.pkg.uid) == 104);
MILLET_STATIC_ASSERT(offsetof(struct millet_up_msg, payload.sig.pid) == 120);

#ifdef __KERNEL__
struct task_struct;
struct millet_sock;

typedef void (*millet_recv_cb)(struct millet_down_msg *msg, unsigned int len);
typedef int (*millet_send_cb)(struct task_struct *task,
			      struct millet_up_msg *msg,
			      struct millet_sock *sock);
typedef void (*millet_init_cb)(struct millet_sock *sock);

int millet_sendto_user(struct task_struct *task, struct millet_up_msg *msg,
		       struct millet_sock *sock);
int millet_sendmsg(unsigned int type, struct task_struct *task,
		   struct millet_up_msg *msg);
int register_millet_hook(unsigned int type, millet_recv_cb recv,
			 millet_send_cb send, millet_init_cb init);
int unregister_millet_hook(unsigned int type);
int init_millet_subsystem(unsigned int type);
void millet_sig(void *unused, int sig, struct task_struct *killer,
		struct task_struct *destination);

extern bool millet_binder_switch;
extern unsigned int millet_debug;
extern unsigned int frozen_uid_min;
extern unsigned long binder_warn_ahead_space;
#endif

#endif /* _XIAOMI_MILLET_H_ */
