/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_FINAL_LINK_H
#define _XIAOMI_METIS_FINAL_LINK_H
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include "metis_lock_wait.h"

struct binder_transaction;
struct plist_node;
struct plist_head;
struct rwsem_waiter;
/* Native android_rvh_try_to_wake_up supplies probe data plus task.
 * Original provider consumer/CFI acceptance remains UNTESTED. */
typedef void (*metis_link_wake_fn)(void *data, struct task_struct *task);
typedef void (*metis_game_wake_fn)(struct task_struct *task,
		unsigned int wake_flags);
extern metis_link_wake_fn mi_try_to_wake_up_func[3];
extern metis_game_wake_fn game_wake_hooks;
extern struct miscdevice metis_link_misc;
extern const struct proc_ops mi_art_mutex_debug_proc_ops;
extern struct proc_dir_entry *mi_art_mutex_entry;
extern spinlock_t mi_art_mutex_lock;
extern s32 mi_art_mutex_owner_pid;
extern u64 mi_art_mutex_set_times, mi_art_mutex_unset_times;
extern u32 binder_boost_msec;
extern u32 mutex_normal_spin_thres, mutex_vip_spin_thres;
extern u32 rwsem_normal_spin_thres, rwsem_vip_spin_thres;
extern u32 reader_biased_jif, read_trylock_reserved_jif, ui_blocked_reader_jif;
extern unsigned long rtg_handoff_min_inter, rtg_rwsem_handoff_jif;

void do_set_binder_vip(struct binder_transaction *transaction,
		struct task_struct *target);
void android_vh_binder_alloc_new_buf_locked(void *data, size_t size,
		size_t *free_async_space, int is_async, bool *should_fail);
void mi_binder_restore_vip_hook(void *data, struct binder_transaction *transaction,
		struct task_struct *task);
void mi_binder_set_vip_hook(void *data, struct binder_transaction *transaction,
		struct task_struct *task);
void mi_alter_futex_plist_add(void *data, struct plist_node *node,
		struct plist_head *head, bool *already_on_hb);
void mi_do_futex(void *data, int command, unsigned int *flags, u32 __user *uaddr2);
void mi_futex_sleep_start(void *data, struct task_struct *task);
void mi_futex_wake_this(void *data, int ret, int nr_wake, int target_nr,
		struct task_struct *task);
void mi_futex_wait_end(void *data, unsigned int flags, u32 bitset);
u32 task_prio_for_rwsem_insert(struct task_struct *task);

/* Native waiter definition is now shared by root's explicitly authorized
 * internal header move, kernel/locking/rwsem.h. No private layout replica. */
void mi_rwsem_read_trylock_failed(void *data, struct rw_semaphore *sem,
		long *count, int *ret);
void mi_rwsem_list_add_hook(void *data, struct rwsem_waiter *waiter,
		struct rw_semaphore *sem, bool *already_on_list);
void do_rwsem_list_insert(struct rwsem_waiter *waiter,
		struct rw_semaphore *sem, bool *already_on_list, int priority);

void metis_link_try_to_wake_up_hook(void *data, struct task_struct *task);
int metis_link_init(void);
void metis_link_exit(void);
long metis_link_ioctl(struct file *file, unsigned int command, unsigned long arg);
int metis_link_mmap(struct file *file, struct vm_area_struct *vma);
int metis_link_open(struct inode *inode, struct file *file);
int metis_link_release(struct inode *inode, struct file *file);
int mi_art_mutex_debug_open(struct inode *inode, struct file *file);
int mi_art_mutex_debug_show(struct seq_file *seq, void *unused);
void register_rvh_mi_try_to_wake_up(unsigned int module, metis_link_wake_fn hook);
void unregister_rvh_mi_try_to_wake_up(unsigned int module);
void register_game_wake_f(metis_game_wake_fn hook);
void unregister_game_wake_f(void);
void mi_mutex_can_spin_on_owner(void *data, struct mutex *lock, int *ret);
void mi_mutex_init(void *data, struct mutex *lock);
void mi_mutex_opt_spin_finish(void *data, struct mutex *lock, bool taken);
void mi_mutex_opt_spin_start(void *data, struct mutex *lock, bool *time_out,
		int *count);
void mi_rwsem_can_spin_on_owner(void *data, struct rw_semaphore *sem, bool *ret);
void mi_rwsem_direct_rsteal(void *data, struct rw_semaphore *sem, bool *steal);
void mi_rwsem_init(void *data, struct rw_semaphore *sem);
void mi_rwsem_opt_spin_finish(void *data, struct rw_semaphore *sem, bool taken);
void mi_rwsem_opt_spin_start(void *data, struct rw_semaphore *sem,
		bool *time_out, int *count, bool check_only);
void mi_rwsem_optimistic_rspin(void *data, struct rw_semaphore *sem,
		long *adjustment, bool *rspin);
u32 rwsem_spin_on_owner(struct rw_semaphore *sem);
void register_cs_hooks(void);
void unregister_cs_hooks(void);
int metis_cs_init(void);
void metis_cs_exit(void);
void register_coldstart_hooks(void);
void unregister_coldstart_hooks(void);
#endif
