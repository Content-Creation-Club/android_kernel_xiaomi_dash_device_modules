/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_LOCK_WAIT_H
#define _XIAOMI_METIS_LOCK_WAIT_H
#include <linux/types.h>
struct task_struct;
struct mutex;
struct mutex_waiter;
struct list_head;
struct rw_semaphore;
struct rt_mutex;
struct percpu_rw_semaphore;

/* Actual stock state/helper owners remain absent, not replaced with defaults. */
extern u32 cs_stat_type; /* Stock .data+0x3fd0; exact original signedness unknown. */
void mutex_reset_data_no_waiter(struct mutex *lock);
/* Raw return W0 is tested at bit0; original C return spelling is unknown. */
u32 do_set_lock_vip(struct task_struct *owner, struct task_struct *waiter,
		     const char *type);
void mi_record_lock_starttime(struct task_struct *task, unsigned long start,
			      const char *type);
void mi_lock_vip_finish_common(struct task_struct *task);

/* Four-argument exported helper, not a native five-argument trace callback. */
void mi_mutex_list_add_hook(struct mutex *lock, struct mutex_waiter *waiter,
			    struct list_head *list, bool *already_on_list);
void mi_mutex_wait_start_hook(void *unused, struct mutex *lock);
void mi_rwsem_write_wait_start_hook(void *unused, struct rw_semaphore *sem);
void mi_rwsem_read_wait_start_hook(void *unused, struct rw_semaphore *sem);
void mi_rwsem_lock_starttime(void *unused, struct rw_semaphore *sem,
			   unsigned long start);
void mi_mutex_lock_starttime(void *unused, struct mutex *lock,
			   unsigned long start);
void mi_rtmutex_lock_starttime(void *unused, struct rt_mutex *lock,
			     unsigned long start);
void mi_pcpu_rwsem_lock_starttime(void *unused, struct percpu_rw_semaphore *sem,
				unsigned long start);
void mi_pcpu_rwsem_wq_add(void *unused, struct percpu_rw_semaphore *sem,
			bool reader);
void mi_rwsem_write_wait_finish_hook(void *unused, struct rw_semaphore *sem);
void mi_lock_rwsem_wake_finish_hook(void *unused, struct rw_semaphore *sem);

/* mi_rwsem_list_add_hook and mi_rwsem_read_trylock_failed stay absent: the
 * native rwsem_waiter definition has no canonical standalone header here. */
#endif
