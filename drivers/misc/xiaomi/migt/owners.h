/* SPDX-License-Identifier: GPL-2.0 */
/* Genuine stock owners and interface metadata; see OWNER_CONTRACT.txt. */
#ifndef XIAOMI_MIGT_OWNERS_H
#define XIAOMI_MIGT_OWNERS_H
#include <linux/miscdevice.h>
#include <linux/moduleparam.h>
#include "subsystem_lifecycle.h"

extern u32 perf_mod, enable_pkg_monitor;
extern u32 choose_load_policy, choose_run_cluster_load, choose_flw_type;
extern u32 choose_wake_state, choose_other_cluster_load, choose_other_cluster_freq;
extern u32 fpercent;
extern int up_thresh_margin, down_thresh_margin;
extern struct proc_dir_entry *migt_dir, *exec_buckets_entry;
/* Reuse the runtime.c owner; entry and runtime share this same lock. */
extern spinlock_t migt_lock;
extern const struct file_operations migt_fops;
extern struct miscdevice migt_misc;

extern const struct kernel_param_ops param_ops_migt_freq;
extern const struct kernel_param_ops param_ops_migt_flag;
extern const struct kernel_param_ops param_ops_exec_buckets;
extern const struct kernel_param_ops param_ops_refer_cluster;
extern const struct kernel_param_ops param_ops_refer_cpu;
extern const struct kernel_param_ops param_ops_refer_cpumask_migt_reserve_cpus_rt;
extern const struct kernel_param_ops param_ops_refer_cpumask_migt_reserve_cpus_mtk_vip;
extern const struct kernel_param_ops param_ops_flw_enable;
extern const struct kparam_array migt_param_array_glk_maxfreq;
extern const struct kparam_array migt_param_array_glk_minfreq;
extern const struct kernel_param migt_parameters[70];
#endif
