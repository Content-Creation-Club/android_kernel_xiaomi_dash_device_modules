/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_FINAL_NATIVE_H
#define _XIAOMI_METIS_FINAL_NATIVE_H
#include <linux/list.h>
#include <linux/sched/task.h>
#include <linux/uaccess.h>

/* Stock out-of-line compiler copies map to the current native APIs used
 * directly by recovered callers, without extra duplicate wrappers:
 *   list_add         00109858 -> linux/list.h list_add
 *   get_task_struct  0011bb1c -> linux/sched/task.h get_task_struct
 *   put_task_struct  00109bf8,0010d24c,0011bb90 -> put_task_struct
 *   _copy_from_user  0011c30c -> linux/uaccess.h _copy_from_user
 * This preserves API semantics, not the old native instruction sequence:
 * refcount hardening, PREEMPT_RT disposal and uaccess instrumentation come
 * from 6.6.144. Their emitted native ABI/runtime remain UNTESTED.
 *
 * Stock trace_mi_wakeup_link 00127510 (.text+1eae8..1eb78) is supplied by
 * the real DECLARE_TRACE(mi_wakeup_link) in metis-wakeup.c. The dormant
 * static-key CPU-online/preemption branch is genuine, not a RET stub.
 */
#endif
