/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_FINAL_CONTROL_H
#define _XIAOMI_METIS_FINAL_CONTROL_H
#include <linux/fs.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include "metis_monitor.h"
#include "metis-final-scheduler.h"
#include "metis_binder_request.h"
#include "metis_cluster_boost.h"

/* Genuine stock owners; root supplies data/defaults, never substitute state. */
extern u32 cluaff_control;
extern unsigned long rebind_task_blist[512], rebind_task_mlist[512];
extern unsigned long rebind_task_llist[512];
extern unsigned long cluster_affinity_uid_blist[3125];
extern unsigned long cluster_affinity_uid_mlist[3125];
extern unsigned long cluster_affinity_uid_llist[3125];
extern u32 mi_rebind_task[4][5], mi_viptask[3];
extern s32 thermal_break_enable, last_traced_uid;
extern unsigned int ui_last_cpu, ren_last_cpu, sf_last_cpu, hwc_last_cpu;
extern s32 show_traced_uid; /* Stock METIS .bss+0x159b0, size4. */
extern s64 vtask_monitor_beg_us, vtask_monitor_end_us, frame_boost_start;
extern unsigned long max_limit_freq[3], thermal_limit_freq[3];
extern s32 comm_monitor_debug, set_static_vip;
extern struct workqueue_struct *my_wq;
extern s32 persist_vip_pids[8];
extern spinlock_t persist_vip_lock;
extern u32 top_task_monitor_window;
extern struct proc_dir_entry *runtime_info_entry;
extern struct notifier_block runtime_info_notifier;
extern const struct proc_ops runtime_info_debug_proc_ops;

/* Stock 0x50 allocation; native work_struct replaces the stock work prefix. */
struct metis_comm_work {
	struct work_struct work;
	struct task_struct *task;
	struct mi_task_struct *state;
	struct mi_task_struct *leader_state;
	bool top_app;
};

void add_to_rebind_task_list(u32 pid, u32 cluster);
void clear_rebind_task_list(u32 pid, int cluster);
int get_rebind_task(char *buffer, const struct kernel_param *param);
int set_rebind_task(const char *value, const struct kernel_param *param);
void rebind_task_req(u32 *pids, u32 count, u32 cluster, bool unbind);
void add_uid_to_list(u32 uid, int cluster);
void del_uid_from_list(u32 uid, int cluster);
int get_clus_affinity_uidlist(char *buffer, const struct kernel_param *param);
int set_clus_affinity_uidlist(const char *value, const struct kernel_param *param);
int uid_type(u32 uid);
int proc_viptask_cpus_set(struct ctl_table *table, int write, void *buffer,
			 size_t *lenp, loff_t *ppos);

int get_mi_viptask(char *buffer, const struct kernel_param *param);
int set_mi_vip_task_req(const char *value, const struct kernel_param *param);
void mi_vip_task_req(u32 *pids, u32 count, u32 duration);
int set_efboost(const char *value, const struct kernel_param *param);
int set_mi_efboost(u32 pid, unsigned long mask, bool clear);
int get_therm_break_enable(char *buffer, const struct kernel_param *param);
int set_therm_break_enable(const char *value, const struct kernel_param *param);
int proc_open_fas(struct inode *inode, struct file *file);
int proc_show_fas(struct seq_file *seq, void *unused);
ssize_t proc_write_fas(struct file *file, const char __user *buffer,
		       size_t count, loff_t *ppos);
long metis_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int metis_mmap(struct file *file, struct vm_area_struct *vma);
int metis_open(struct inode *inode, struct file *file);
int metis_release(struct inode *inode, struct file *file);
void metis_set_vip_thread(struct task_struct *task, u32 kind, u32 seconds);
void metis_update_top_task_info(struct task_struct *task, struct rq *rq,
			       int running, u64 wallclock, u64 previous);

bool str2_in_str1(const char *haystack, const char *needle);
bool is_main_thread_depend(struct mi_task_struct *state);
int kernel_set_static_vip(struct task_struct *task);
int func_set_static_vip(const char *value, const struct kernel_param *param);
void compare_task_comm_work_func(struct work_struct *work);
void setup_task_wk_func(struct work_struct *work);
void probe_task_rename(void *unused, struct task_struct *task, bool exec);
void probe_android_vh_cgroup_set_task(void *unused, int result,
				    struct task_struct *task);
int metis_comm_monitor_init(void);
void metis_comm_monitor_exit(void);
int __set_common_persist_vip(s32 pid, struct task_struct *task);
void metis_set_common_persist_vip(struct task_struct *task);
int set_common_persist_vip_task(const char *value,
			      const struct kernel_param *param);
int get_common_persist_vip_task(char *buffer, const struct kernel_param *param);
int set_clear_persist_vip_task(const char *value,
			     const struct kernel_param *param);
int metis_common_api_init(void);
void metis_common_api_exit(void);
void history_task_info_init(void);
int metis_doublecyc_init(void);
void metis_doublecyc_exit(void);
int runtime_info_debug_open(struct inode *inode, struct file *file);
int runtime_info_debug_show(struct seq_file *seq, void *unused);
void do_print_record_abnorm_task_info(struct seq_file *seq, u32 begin,
				    u32 end, int kind);
int kmsg_runtime_info_debug_show(struct notifier_block *nb,
				 unsigned long event, void *data);
#endif
