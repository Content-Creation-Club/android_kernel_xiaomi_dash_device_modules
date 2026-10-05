/* SPDX-License-Identifier: GPL-2.0 */
/* Source-only userspace contracts; native build/runtime untested. */
#ifndef XIAOMI_MIGT_USER_API_H
#define XIAOMI_MIGT_USER_API_H
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include "migt.h"

/* Genuine local state supplied by owners.c and stock lifecycle algorithms. */
extern u32 first_lunch_fps, itask_detect_interval;
extern int render_pid, render_update, high_resolution_enable;
extern u32 over_thresh_count, last_ui_traced_uid;
extern u8 ceiling_enable;
extern unsigned long ceiling_next_update;
extern u32 ceiling_slack_ms;
extern struct delayed_work ceiling_rem, ceiling_work, migt_work;
/* Stock local symbol hrtimer .bss+0x120, separate from vtask_timer. */
extern struct hrtimer migt_frame_timer;
/* Real imported mi_schedule state; no local definition replaces the provider. */
extern u32 show_traced_uid;
/* Stock MIGT .bss+0x24, __param ops_uint: this is a local owner. */
extern u32 force_reset_runtime;

long migt_ioctl(struct file *file, unsigned int command, unsigned long argument);
int migt_open(struct inode *inode, struct file *file);
int migt_release(struct inode *inode, struct file *file);
struct vm_area_struct;
int migt_mmap(struct file *file, struct vm_area_struct *vma);
int exec_buckets_set(const char *value, const struct kernel_param *param);
int exec_buckets_get(char *buffer, const struct kernel_param *param);
int exec_buckets_show(struct seq_file *seq, void *unused);
int exec_buckets_open(struct inode *inode, struct file *file);
struct migt_rank_entry { u64 unknown_00, score; };
int cmp(const void *left, const void *right);
void dump_his_load_info(int index, struct seq_file *seq);
int migt_frame_history_load_show(struct seq_file *seq, void *unused);
int migt_frame_history_load_open(struct inode *inode, struct file *file);
int migt_frame_cpu_load_show(struct seq_file *seq, void *unused);
int migt_frame_cpu_load_open(struct inode *inode, struct file *file);
int migt_game_history_load_show(struct seq_file *seq, void *unused);
int migt_game_history_load_open(struct inode *inode, struct file *file);
int migt_game_cpu_load_show(struct seq_file *seq, void *unused);
int migt_game_cpu_load_open(struct inode *inode, struct file *file);
extern const struct proc_ops migt_exec_buckets_proc_ops;
extern const struct proc_ops migt_frame_history_proc_ops;
extern const struct proc_ops migt_frame_cpu_proc_ops;
void create_game_load_proc(struct proc_dir_entry *parent);
void delete_game_load_proc(void);
int proc_glk_dointvec(struct ctl_table *table, int write, void *buffer,
		size_t *length, loff_t *position, proc_handler *handler);
int proc_glk_dotload(struct ctl_table *table, int write, void *buffer,
		size_t *length, loff_t *position);
int proc_glk_dofreq(struct ctl_table *table, int write, void *buffer,
		size_t *length, loff_t *position);
void trigger_ceiling_limit(void);
#endif
