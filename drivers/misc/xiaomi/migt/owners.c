// SPDX-License-Identifier: GPL-2.0
/* Stock bytes/relocations as native objects; native build untested. */
#include <linux/module.h>
#include "owners.h"

/* .data+0x00..0x48; existing boost/policy owners retain their own defaults. */
u32 perf_mod = 1;
u32 first_lunch_fps = 100;
u32 ceiling_slack_ms = 1000;
int high_resolution_enable = 1;
u32 itask_detect_interval = 5000;
int render_update = 1;
u32 enable_pkg_monitor = 1;
u32 stask_candidate_num = 2;
u32 vip_task_max_num = 15;
/* .data+0xe8..0xf8. render_pid is deliberately negative before first frame. */
int render_pid = -1;
u32 task_enqueue_time_toolong = 3000;
u32 pick_stask_policy = 1;
/* .data+0x2d8..0x2f4. These controls are exposed even where stock ignores them. */
u32 choose_load_policy = 10;
u32 choose_run_cluster_load = 2;
u32 choose_flw_type = 5;
u32 choose_wake_state = 1;
int up_thresh_margin = 50;
int down_thresh_margin = 200;

/* Genuine .bss, in original address order. INIT_* belongs to module entry,
 * except objects for which the stock entry itself omits initialization. */
u32 over_thresh_count;                         /* +0x008, 4 */
u32 force_cluster_sched_enable;                /* +0x010, 4 */
u32 set_render_as_stask;                        /* +0x018, 4 */
u32 ip_task_max_num;                            /* +0x01c, 4 */
u32 need_reset_runtime_everytime;               /* +0x020, 4 */
u32 force_reset_runtime;                        /* +0x024, 4; LOCAL uint */
u32 force_forbidden_walt_lb;                     /* +0x028, 4 */
struct work_struct irq_boost_work;              /* +0x0a0, 0x30 */
struct hrtimer vtask_timer;                     /* +0x0d0, 0x48 */
u64 cpus_visit;                                 /* cpus_visit.0 +0x118, 8 */
struct hrtimer migt_frame_timer;                /* hrtimer +0x120, 0x48 */
struct workqueue_struct *migt_wq;                /* +0x168, 8 */
struct proc_dir_entry *migt_dir;                 /* +0x170, 8 */
struct proc_dir_entry *exec_buckets_entry;       /* +0x178, 8 */
struct delayed_work migt_work;                  /* +0x180, 0x88 */
struct delayed_work migt_rem;                   /* +0x208, 0x88 */
struct delayed_work ceiling_work;               /* +0x290, 0x88 */
struct delayed_work ceiling_rem;                /* +0x318, 0x88 */
unsigned long ceiling_next_update;             /* +0x3a8, 8 */
u8 ceiling_enable;                             /* +0x3b0, 1 */
u32 last_ui_traced_uid;                         /* +0x3b4, 4 */
int migt_has_work_todo;                         /* +0x420, 4 */
int migt_init_sucess;                           /* +0x424, 4 */
int migt_render_debug;                         /* second migt_debug +0x428, 4 */
int g_work;                                    /* +0x42c, 4 */
struct task_struct *migt_thread;                /* +0x430, 8 */
struct migt_render_info render_info[2];         /* +0x440, 0x80; native locks */
struct ctl_table_header *migt_sched_header;     /* +0x540, 8 */
struct migt_rank_entry user_array[1024];        /* +0x548, 0x4000 */
u32 choose_other_cluster_load;                 /* +0x4550, 4 */
u32 choose_other_cluster_freq;                 /* +0x4554, 4 */
u32 fpercent;                                  /* +0x4558, 4 */
u32 flw_work_in_process;                        /* +0x8bfc, 4 */
struct proc_dir_entry *frame_load_dir;          /* +0x8c00, 8 */
struct proc_dir_entry *migt_frame_history_entry;/* +0x8c08, 8 */
struct proc_dir_entry *migt_frame_cpu_entry;    /* +0x8c10, 8 */
struct irq_work flw_cpufreq_irq_work;           /* +0x8c18, 0x20 */
struct ctl_table_header *game_load_header;      /* +0x91e0, 8 */

/* .rodata+0x468, 0x108 bytes: only the six recorded pointer relocations.
 * Native named members replace the stock 6.6.118 file_operations offsets. */
const struct file_operations migt_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = migt_ioctl,
	.compat_ioctl = migt_ioctl,
	.mmap = migt_mmap,
	.open = migt_open,
	.release = migt_release,
};

/* .data+0x68, 0x50 bytes: minor=255, name/fops relocations, all else zero.
 * The mode remains zero: misc core chooses its default device permissions. */
struct miscdevice migt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "migt",
	.fops = &migt_fops,
};

/* .data+0x118, 7 x 0x40. Keep proc_dointvec on uint storage, as in stock;
 * do not add bounds, unsigned handlers, or substitute the first migt_debug. */
#define MIGT_SYSCTL(_name, _data, _length, _handler) \
	{ .procname = (_name), .data = (_data), .maxlen = (_length), \
	  .mode = 0644, .proc_handler = (_handler) }
struct ctl_table migt_table[7] = {
	MIGT_SYSCTL("enable_pkg_monitor", &enable_pkg_monitor, 4, proc_dointvec),
	MIGT_SYSCTL("migt_sched_debug", &migt_render_debug, 4, proc_dointvec),
	MIGT_SYSCTL("render_stask", &set_render_as_stask, 4, proc_dointvec),
	MIGT_SYSCTL("stask_candidate_num", &stask_candidate_num, 4, proc_dointvec),
	MIGT_SYSCTL("vip_task_max_num", &vip_task_max_num, 4, proc_dointvec),
	MIGT_SYSCTL("ip_task_max_num", &ip_task_max_num, 4, proc_dointvec),
	{ },
};
/* .data+0x330, 11 x 0x40. The distinct load perf_mod remains its own owner. */
struct ctl_table game_load_table[11] = {
	MIGT_SYSCTL("game_load_choose_policy", &sysctl_game_load_choose_policy,
		4, proc_dointvec),
	MIGT_SYSCTL("glk_disable", &glk_disable, 4, proc_dointvec),
	MIGT_SYSCTL("glk_freq_limit_walt", &glk_freq_limit_walt, 4, proc_dointvec),
	MIGT_SYSCTL("game_hispeed_load", glk_highspeed_load, 12, proc_glk_dotload),
	MIGT_SYSCTL("game_lowspeed_load", glk_lowspeed_load, 12, proc_glk_dotload),
	MIGT_SYSCTL("game_minfreq_limit", glk_minfreq, 12, proc_glk_dofreq),
	MIGT_SYSCTL("freq_break_enable", &glk_fbreak_enable, 4, proc_dointvec),
	MIGT_SYSCTL("game_maxfreq_limit", glk_maxfreq, 12, proc_glk_dofreq),
	MIGT_SYSCTL("perf_mod", &game_load_perf_mod, 4, proc_dointvec),
	MIGT_SYSCTL("load_debug", &game_load_debug, 4, proc_dointvec),
	{ },
};
#undef MIGT_SYSCTL

/* .rodata+0x2d8/0x2f8/0x328/0x360/0x5d0/0x678/0x6b8/0x7b8.
 * Raw bytes are zero; each ops contains only its observed set/get relocs. */
const struct kernel_param_ops param_ops_migt_freq = {
	.set = set_migt_freq, .get = get_migt_freq,
};
const struct kernel_param_ops param_ops_migt_flag = {
	.set = set_migt_flag, .get = get_migt_flag,
};
const struct kernel_param_ops param_ops_exec_buckets = {
	.set = exec_buckets_set, .get = exec_buckets_get,
};
const struct kernel_param_ops param_ops_refer_cluster = {
	.set = set_prefer_cluster_with_check, .get = param_get_int,
};
const struct kernel_param_ops param_ops_refer_cpu = {
	.set = set_prefer_cpu_with_check, .get = param_get_int,
};
const struct kernel_param_ops param_ops_refer_cpumask_migt_reserve_cpus_rt = {
	.set = set_migt_reserve_cpus_rt, .get = get_migt_reserve_cpus_rt,
};
const struct kernel_param_ops param_ops_refer_cpumask_migt_reserve_cpus_mtk_vip = {
	.set = set_migt_reserve_cpus_mtk_vip, .get = get_migt_reserve_cpus_mtk_vip,
};
const struct kernel_param_ops param_ops_flw_enable = {
	.set = set_flw_enable, .get = get_flw_enable,
};

/* .rodata+0x260/0x290: raw max=3, elemsize=4, num=NULL, ops/elem relocs. */
const struct kparam_array migt_param_array_glk_maxfreq = {
	.max = 3, .elemsize = 4, .ops = &param_ops_uint, .elem = glk_maxfreq,
};
const struct kparam_array migt_param_array_glk_minfreq = {
	.max = 3, .elemsize = 4, .ops = &param_ops_uint, .elem = glk_minfreq,
};

/* Genuine __param: 70 x 0x28, preserved order and name/ops/arg edges.
 * All permissions are 0644, level=-1, flags=0. The section is metadata;
 * module.c supplies entry/exports; no parent/config/package activation. */
#define MIGT_PARAM(_name, _ops, _arg) \
	{ .name = (_name), .mod = THIS_MODULE, .ops = (_ops), \
	  .perm = 0644, .level = -1, .flags = 0, .arg = (_arg) }
const struct kernel_param migt_parameters[70] __used __section("__param")
	__aligned(__alignof__(struct kernel_param)) = {
	MIGT_PARAM("perf_mod", &param_ops_uint, &perf_mod),
	MIGT_PARAM("first_lunch_fps", &param_ops_uint, &first_lunch_fps),
	MIGT_PARAM("ceiling_slack_ms", &param_ops_uint, &ceiling_slack_ms),
	MIGT_PARAM("ceiling_from_user", &param_ops_uint, &ceiling_from_user),
	MIGT_PARAM("high_resolution_enable", &param_ops_uint, &high_resolution_enable),
	MIGT_PARAM("migt_debug", &param_ops_uint, &migt_debug),
	MIGT_PARAM("migt_ms", &param_ops_uint, &migt_ms),
	MIGT_PARAM("migt_thresh", &param_ops_uint, &migt_thresh),
	MIGT_PARAM("itask_detect_interval", &param_ops_uint, &itask_detect_interval),
	MIGT_PARAM("over_thresh_count", &param_ops_uint, &over_thresh_count),
	MIGT_PARAM("boost_policy", &param_ops_uint, &boost_policy),
	MIGT_PARAM("cpu_boost_cycle", &param_ops_uint, &cpu_boost_cycle),
	MIGT_PARAM("render_update", &param_ops_uint, &render_update),
	MIGT_PARAM("force_cluster_sched_enable", &param_ops_uint, &force_cluster_sched_enable),
	MIGT_PARAM("migt_viptask_thresh", &param_ops_uint, &migt_viptask_thresh),
	MIGT_PARAM("fps_mexec_update_policy", &param_ops_uint, &fps_mexec_update_policy),
	MIGT_PARAM("fps_variance_ratio", &param_ops_uint, &fps_variance_ratio),
	MIGT_PARAM("enable_pkg_monitor", &param_ops_uint, &enable_pkg_monitor),
	MIGT_PARAM("set_render_as_stask", &param_ops_uint, &set_render_as_stask),
	MIGT_PARAM("stask_candidate_num", &param_ops_uint, &stask_candidate_num),
	MIGT_PARAM("vip_task_max_num", &param_ops_uint, &vip_task_max_num),
	MIGT_PARAM("ip_task_max_num", &param_ops_uint, &ip_task_max_num),
	MIGT_PARAM("need_reset_runtime_everytime", &param_ops_uint, &need_reset_runtime_everytime),
	MIGT_PARAM("force_reset_runtime", &param_ops_uint, &force_reset_runtime),
	MIGT_PARAM("force_forbidden_walt_lb", &param_ops_uint, &force_forbidden_walt_lb),
	MIGT_PARAM("force_viptask_select_rq", &param_ops_uint, &force_viptask_select_rq),
	MIGT_PARAM("glk_disable", &param_ops_uint, &glk_disable),
	MIGT_PARAM("glk_fbreak_enable", &param_ops_uint, &glk_fbreak_enable),
	MIGT_PARAM("glk_freq_limit_start", &param_ops_uint, &glk_freq_limit_start),
	MIGT_PARAM("glk_freq_limit_walt", &param_ops_uint, &glk_freq_limit_walt),
	MIGT_PARAM("glk_maxfreq", &param_array_ops, (void *)&migt_param_array_glk_maxfreq),
	MIGT_PARAM("glk_minfreq", &param_array_ops, (void *)&migt_param_array_glk_minfreq),
	MIGT_PARAM("gtask_override_schedboost", &param_ops_uint, &gtask_override_schedboost),
	MIGT_PARAM("affinity_only", &param_ops_uint, &affinity_only),
	MIGT_PARAM("migt_freq", &param_ops_migt_freq, NULL),
	MIGT_PARAM("migt_ceiling_freq", &param_ops_migt_freq, NULL),
	MIGT_PARAM("add_ip_group", &param_ops_migt_flag, NULL),
	MIGT_PARAM("add_vip_group", &param_ops_migt_flag, NULL),
	MIGT_PARAM("clean_user_group", &param_ops_migt_flag, NULL),
	MIGT_PARAM("exec_buckets", &param_ops_exec_buckets, NULL),
	MIGT_PARAM("stask_prefer_cluster", &param_ops_refer_cluster, &stask_prefer_cluster),
	MIGT_PARAM("render_prefer_cluster", &param_ops_refer_cluster, &render_prefer_cluster),
	MIGT_PARAM("vip_prefer_cluster", &param_ops_refer_cluster, &vip_prefer_cluster),
	MIGT_PARAM("ip_prefer_cluster", &param_ops_refer_cluster, &ip_prefer_cluster),
	MIGT_PARAM("task_enqueue_time_toolong", &param_ops_uint, &task_enqueue_time_toolong),
	MIGT_PARAM("choose_cpu_exclusive_enable", &param_ops_uint, &choose_cpu_exclusive_enable),
	MIGT_PARAM("custom_second_group", &param_ops_uint, &custom_second_group),
	MIGT_PARAM("stask_prefer_cpu", &param_ops_refer_cpu, &stask_prefer_cpu),
	MIGT_PARAM("render_prefer_cpu", &param_ops_refer_cpu, &render_prefer_cpu),
	MIGT_PARAM("vip_prefer_cpu", &param_ops_refer_cpu, &vip_prefer_cpu),
	MIGT_PARAM("ip_prefer_cpu", &param_ops_refer_cpu, &ip_prefer_cpu),
	MIGT_PARAM("stask_affine_cpu", &param_ops_refer_cpu, &stask_affine_cpu),
	MIGT_PARAM("render_affine_cpu", &param_ops_refer_cpu, &render_affine_cpu),
	MIGT_PARAM("vip_affine_cpu", &param_ops_refer_cpu, &vip_affine_cpu),
	MIGT_PARAM("ip_affine_cpu", &param_ops_refer_cpu, &ip_affine_cpu),
	MIGT_PARAM("migt_reserve_cpus_rt", &param_ops_refer_cpumask_migt_reserve_cpus_rt,
		&migt_reserve_cpus_rt),
	MIGT_PARAM("migt_reserve_cpus_mtk_vip", &param_ops_refer_cpumask_migt_reserve_cpus_mtk_vip,
		&migt_reserve_cpus_mtk_vip),
	MIGT_PARAM("flw_debug", &param_ops_uint, &flw_debug),
	MIGT_PARAM("choose_load_policy", &param_ops_uint, &choose_load_policy),
	MIGT_PARAM("flw_freq_enable", &param_ops_uint, &flw_freq_enable),
	MIGT_PARAM("choose_other_cluster_load", &param_ops_uint, &choose_other_cluster_load),
	MIGT_PARAM("choose_other_cluster_freq", &param_ops_uint, &choose_other_cluster_freq),
	MIGT_PARAM("choose_run_cluster_load", &param_ops_uint, &choose_run_cluster_load),
	MIGT_PARAM("choose_flw_type", &param_ops_uint, &choose_flw_type),
	MIGT_PARAM("target_delta", &param_ops_uint, &target_delta),
	MIGT_PARAM("choose_wake_state", &param_ops_uint, &choose_wake_state),
	MIGT_PARAM("fpercent", &param_ops_uint, &fpercent),
	MIGT_PARAM("up_thresh_margin", &param_ops_int, &up_thresh_margin),
	MIGT_PARAM("down_thresh_margin", &param_ops_int, &down_thresh_margin),
	MIGT_PARAM("flw_enable", &param_ops_flw_enable, NULL),
};
#undef MIGT_PARAM

/* Exact original .modinfo parmtype strings: 45 uint, two arrays, two int.
 * Callback-only parameters have no parmtype records in the stock object. */
MODULE_INFO(parmtype, "perf_mod:uint");
MODULE_INFO(parmtype, "first_lunch_fps:uint");
MODULE_INFO(parmtype, "ceiling_slack_ms:uint");
MODULE_INFO(parmtype, "ceiling_from_user:uint");
MODULE_INFO(parmtype, "high_resolution_enable:uint");
MODULE_INFO(parmtype, "migt_debug:uint");
MODULE_INFO(parmtype, "migt_ms:uint");
MODULE_INFO(parmtype, "migt_thresh:uint");
MODULE_INFO(parmtype, "itask_detect_interval:uint");
MODULE_INFO(parmtype, "over_thresh_count:uint");
MODULE_INFO(parmtype, "boost_policy:uint");
MODULE_INFO(parmtype, "cpu_boost_cycle:uint");
MODULE_INFO(parmtype, "render_update:uint");
MODULE_INFO(parmtype, "force_cluster_sched_enable:uint");
MODULE_INFO(parmtype, "migt_viptask_thresh:uint");
MODULE_INFO(parmtype, "fps_mexec_update_policy:uint");
MODULE_INFO(parmtype, "fps_variance_ratio:uint");
MODULE_INFO(parmtype, "enable_pkg_monitor:uint");
MODULE_INFO(parmtype, "set_render_as_stask:uint");
MODULE_INFO(parmtype, "stask_candidate_num:uint");
MODULE_INFO(parmtype, "vip_task_max_num:uint");
MODULE_INFO(parmtype, "ip_task_max_num:uint");
MODULE_INFO(parmtype, "need_reset_runtime_everytime:uint");
MODULE_INFO(parmtype, "force_reset_runtime:uint");
MODULE_INFO(parmtype, "force_forbidden_walt_lb:uint");
MODULE_INFO(parmtype, "force_viptask_select_rq:uint");
MODULE_INFO(parmtype, "glk_disable:uint");
MODULE_INFO(parmtype, "glk_fbreak_enable:uint");
MODULE_INFO(parmtype, "glk_freq_limit_start:uint");
MODULE_INFO(parmtype, "glk_freq_limit_walt:uint");
MODULE_INFO(parmtype, "glk_maxfreq:array of uint");
MODULE_INFO(parmtype, "glk_minfreq:array of uint");
MODULE_INFO(parmtype, "gtask_override_schedboost:uint");
MODULE_INFO(parmtype, "affinity_only:uint");
MODULE_INFO(parmtype, "task_enqueue_time_toolong:uint");
MODULE_INFO(parmtype, "choose_cpu_exclusive_enable:uint");
MODULE_INFO(parmtype, "custom_second_group:uint");
MODULE_INFO(parmtype, "flw_debug:uint");
MODULE_INFO(parmtype, "choose_load_policy:uint");
MODULE_INFO(parmtype, "flw_freq_enable:uint");
MODULE_INFO(parmtype, "choose_other_cluster_load:uint");
MODULE_INFO(parmtype, "choose_other_cluster_freq:uint");
MODULE_INFO(parmtype, "choose_run_cluster_load:uint");
MODULE_INFO(parmtype, "choose_flw_type:uint");
MODULE_INFO(parmtype, "target_delta:uint");
MODULE_INFO(parmtype, "choose_wake_state:uint");
MODULE_INFO(parmtype, "fpercent:uint");
MODULE_INFO(parmtype, "up_thresh_margin:int");
MODULE_INFO(parmtype, "down_thresh_margin:int");
