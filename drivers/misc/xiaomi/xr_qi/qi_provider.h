/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _XIAOMI_QI_PROVIDER_H
#define _XIAOMI_QI_PROVIDER_H
#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
struct task_struct;
struct qi_qs_state { u32 flags; u32 pad; unsigned long deadline; };
struct qi_event { struct task_struct *from; struct task_struct *to; u32 flags; u32 schedattrs; };
/* Stock .text init_task_qi 0x53b0 / allocation 0x540c: five 0x20 slots. */
struct qi_inherit_slot {
	struct list_head entries;
	spinlock_t lock;
	u8 reserved[12];
};
struct task_qi {
	spinlock_t lock;                         /* allocation +0x08 */
	u32 inherit_mask;                         /* +0x0c */
	u32 static_attrs;                         /* +0x10 */
	u32 dynamic_attrs;                        /* +0x14 */
	u32 process_attrs;                        /* +0x18 */
	u32 futex_word;                           /* +0x1c */
	unsigned long inherit_deadline;           /* +0x20 */
	struct qi_inherit_slot slots[5];          /* +0x28..0xc8 */
	u64 mutex_spin_start;                     /* +0xc8 */
	u64 reserved_d0;                          /* +0xd0 */
};
struct mqos_task {
	u64 flags;                               /* +0x00 */
	struct task_qi qi;                        /* +0x08 */
	struct qi_qs_state qs;                    /* +0xd8 */
};
struct mqos_task *get_alloc_mqos_task(struct task_struct *);
struct task_qi *get_alloc_task_qi(struct task_struct *);
struct task_qi *get_task_qi(struct task_struct *);
int get_proc_static_qos_lvl(struct task_struct *);
u32 get_static_qos_lvl(struct task_struct *);
u32 get_dyn_qos_lvl(struct task_struct *);
u32 get_qos_lvl(struct task_struct *);
u32 is_qi_feat_enabled(unsigned long bit);
bool is_qos_task(struct task_struct *);
bool is_qos_rt_task(struct task_struct *);
void qi_intf_schedinfo_update(struct qi_event *);
void qi_intf_schedinfo_reset(struct qi_event *);
int register_qievent_notifier(struct notifier_block *);
int unregister_qievent_notifier(struct notifier_block *);
int call_qievent_notifiers(unsigned long event, struct qi_event *);
int do_qos_inherit(struct task_struct *from, struct task_struct *to, void *cookie, u32 kind);
int undo_qos_inherit(struct task_struct *task, struct task_qi *qi, void *cookie, u32 kind);
#endif
