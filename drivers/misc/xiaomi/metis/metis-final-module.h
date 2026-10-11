/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_FINAL_MODULE_H
#define _XIAOMI_METIS_FINAL_MODULE_H
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/syscore_ops.h>

extern struct miscdevice metis_misc;
extern struct proc_dir_entry *proc_fas_file;
extern const struct proc_ops proc_fas_op;
extern struct syscore_ops fas_syscore_ops;
int fas_init(void);
void fas_exit(void);
/* Literal RET body; unsigned-int spelling inferred from stock KCFI family
 * 019c0cac shared with set_foreground_app and identity setters. No caller
 * survives in the retained module, and native ABI remains UNTESTED. */
void set_foreground_pid(unsigned int pid);
#endif
