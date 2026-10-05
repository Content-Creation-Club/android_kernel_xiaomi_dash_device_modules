/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _XIAOMI_MI_SCHEDULE_H
#define _XIAOMI_MI_SCHEDULE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/percpu.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>

struct task_struct;
struct user_struct;
struct rq;
struct mi_user_struct;

#define MI_TASK_OEM_MAGIC 0xabcdabcdacbdadbcULL
#define MI_USER_OEM_MAGIC 0xdcba1234abcd4321ULL
#define MI_STATE_MAGIC 0xacbdacbdULL
#define MI_TASK_STATE_SIZE 0x4e8
#define MI_RQ_STATE_SIZE 0x1f0
/* Unnamed stock fields retain their offsets instead of invented semantics. */
#define MI_FIELD(object, type, offset) \
	(*(type *)((u8 *)(object) + (offset)))

/* Provider initialization: mi_task_fork .text+0x241c..0x2580;
 * xr-qs uses flags_0xc0 and deadline_0x290. Remaining unnamed storage is
 * reserved for stock metis/migt state, not a promise of initialized bytes. */
struct mi_task_struct {
	union {
		u8 raw[MI_TASK_STATE_SIZE];
		struct {
			u64 magic;                     /* 0x000 */
			u64 guard;                     /* 0x008 */
			u8 unknown_010[0xa8];
			u8 user_linked;                /* 0x0b8 */
			u8 work_pending;               /* 0x0b9 */
			u8 unknown_ba;
			u8 unknown_bb;
			u32 uid;                       /* 0x0bc */
			u32 flags_0xc0;
			u32 unknown_c4;
			u32 unknown_c8;
			u32 unknown_cc;
			u64 initial_exec_runtime;      /* 0x0d0 */
			u64 unknown_d8;
			struct task_struct *task;      /* 0x0e0 */
			struct mi_user_struct *user;   /* 0x0e8 */
			struct list_head user_node;   /* 0x0f0 */
			u8 unknown_100[0x10];
			u64 sup_runtime[6];            /* 0x110 */
			u64 big_runtime[6];            /* 0x140 */
			u64 little_runtime[6];         /* 0x170 */
			u8 unknown_1a0[0xb8];
			struct rb_node rb_node_258;
			u32 rb_magic_270;
			u32 unknown_274;
			u8 unknown_278[0x18];
			u64 deadline_0x290;
			u8 unknown_298[0x28];
			struct list_head node_2c0;
			u8 unknown_2d0[0x218];
		};
	};
};

struct mi_user_struct {
	u32 initialized;                       /* 0x00 */
	u32 unknown_04;
	u64 magic;                             /* 0x08 */
	rwlock_t lock;                         /* 0x10 */
	u32 unknown_14;
	struct list_head tasks;                /* 0x18 */
	u64 sup_runtime[6];                    /* 0x28 */
	u64 big_runtime[6];                    /* 0x58 */
	u64 little_runtime[6];                 /* 0x88 */
	struct user_struct *user;              /* 0xb8 */
};

/* All 0x1f0 bytes are exported as stock per-CPU data. Stock free-task uses
 * rb_root_cached at 0x160 and spinlock at 0x1c8. The other fields are consumed
 * by metis and retain raw access pending that module's reconstruction. */
struct mi_rq { u8 raw[MI_RQ_STATE_SIZE] __aligned(8); };
DECLARE_PER_CPU_SHARED_ALIGNED(struct mi_rq, mi_runqueues);

typedef void (*mi_sched_load_update_fn)(struct task_struct *task, int cpu,
		int event, u64 wallclock, u64 delta_exec);
struct mi_task_struct *get_mi_task_struct(struct task_struct *task);
struct mi_task_struct *get_mi_task_struct_unsafe(struct task_struct *task);
struct mi_user_struct *get_mi_user_struct(struct user_struct *user);
bool mi_task_exiting(struct task_struct *task);
void mi_task_fork(void *unused, struct task_struct *task);
void free_mi_task_struct(void *unused, struct task_struct *task);
void mi_sched_tick(void *unused, struct rq *rq);
int get_cluster_type(unsigned int cpu);
u32 is_32B_user(int uid);
bool pkg_enable(void);
void update_pkg_load(struct task_struct *task, int cpu, int event,
		u64 wallclock, u64 delta_exec);
void register_sched_load_update_hook(int module, mi_sched_load_update_fn hook);
void unregister_sched_load_update_hook(int module);
#endif
