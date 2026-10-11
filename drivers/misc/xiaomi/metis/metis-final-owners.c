// SPDX-License-Identifier: GPL-2.0-only
/* Genuine METIS owners. Defaults come from stock loaded .data/.data..percpu
 * bytes and the ELF's zero-initialized .bss, not substitute policy defaults.
 * Native kernel objects use this tree's own definitions and initializers. */
#include <linux/cache.h>
#include "metis-final-owners.h"

/* .bss+0x0..0x4f: link state. */
s32 link_debug, mi_link_enable, mi_async_binder_link_enable;
metis_link_wake_fn mi_try_to_wake_up_func[3];
u32 mi_lock_blocked_pid;
struct proc_dir_entry *mi_art_mutex_entry;
u64 mi_art_mutex_set_times, mi_art_mutex_unset_times;
s32 mi_art_mutex_owner_pid;
DEFINE_SPINLOCK(mi_art_mutex_lock);

/* .bss+0x50..0x155ff and .data+0x50..0x58: cluster affinity. */
u32 minor_window_app, tsched_debug;
metis_game_wake_fn game_wake_hooks;
u32 bug_detect = 1;
u32 cluaff_control = 1;
s32 vip_task_max_cpu_nr = 7;
struct cpumask viptask_use_cpumask;
struct ctl_table_header *clu_aff_header;
u32 mi_rebind_task[4][5];
unsigned long rebind_task_mlist[512], rebind_task_llist[512];
unsigned long rebind_task_blist[512];
struct cpumask cluster_affinity_cpumask[4];
unsigned long cluster_affinity_uid_mlist[3125];
unsigned long cluster_affinity_uid_llist[3125];
unsigned long cluster_affinity_uid_blist[3125];
u8 cluster_core_init_suc;
struct cpumask cluster_core_mask[4];

/* .data+0xe0..0x157 and .bss+0x15600..0x15997: frequency boost. */
u32 mi_boost_duration = 40;
unsigned long thermal_limit_freq[3] = { ~0UL, ~0UL, ~0UL };
s32 mi_freq_enable = 1;
unsigned long max_limit_freq[3] = { ~0UL, ~0UL, ~0UL };
u32 user_min_freq[3] = { 0, 1000000, 1100000 };
struct notifier_block init_fboost_notifier = {
	.notifier_call = metis_freq_qos_request_init,
};
unsigned int n_freq = 3;
u32 min_cluster_freqs[3] = { 1324800, 1785600, 1708800 };
DECLARE_WORK(finish_fboost_work, finish_fboost_workfn);
s32 fboost_debug, mi_switch_enable;
u32 in_perf_mod, perf_min_freq[3];
metis_update_perflock_freq_fn update_perflock_freq_func;
metis_perflock_last_freqs_fn get_perflock_last_freqs_func;
metis_corectl_boost_fn oem_corectl_boost_func;
struct metis_fboost_execution fboost_exe_mode;
DEFINE_RAW_SPINLOCK(fboost_pending_lock);
u8 fboost_pending;
struct metis_cluster_boost_state bcluster_stat[3] __aligned(8);
s32 cluster_states[3];
struct task_struct *fboost_thread;
unsigned long cpus_visit_0; /* Original local cpus_visit.0. */
struct metis_cluster_boost_timer boost_timers[3];
struct metis_cluster_adjust_work rem_cluster_works[3];
struct workqueue_struct *metis_wq;
struct metis_cluster_max_notifier nb_max_limit[3];

/* .data+0x3db0..0x3dc7 and .bss+0x15998..0x15bcf: FAS. */
u32 binder_boost_msec = 10;
u32 fperiod = 8;
s32 preempt_sysprocess = 1;
u32 task_enqueue_time_toolong = 3000;
s32 ui_run_cpu = -1, render_run_cpu = -1;
char version[64] = "4.0";
metis_detect_fn choose_cpu_overutiled[3];
s32 show_traced_uid;
unsigned int metis_debug;
s32 mi_fboost_enable;
u32 limit_bgtask_sched;
s32 thermal_break_enable;
u32 force_forbidden_walt_lb, force_viptask_select_rq;
u32 force_cluster_sched_enable;
unsigned int more_vip_flag_print;
s32 speed_touch_producer_in_boost, speed_touch_consumer_in_boost;
metis_walt_fn mi_walt_get_indicies_func[3];
metis_energy_fn wake_render_func[3];
metis_fair_task_fn mi_enqueue_task_fair_func[3];
metis_fair_task_fn mi_dequeue_task_fair_func[3];
void *get_cpu_util_without; /* UNKNOWN callback typedef; storage only. */
metis_skip_cpus_fn game_migt_skip_cpus_metis_hook;
u32 stask_prefer_cluster, render_prefer_cluster, vip_prefer_cluster;
metis_sched_enable_fn mi_sched_enable[3];
atomic_t mi_sdk_eboost_vip_num = ATOMIC_INIT(0);
unsigned long mi_link_boost_jif;
s32 speed_touch_enable, trace_motion_event;
s64 touch_up_start;
atomic_t mi_dynamic_vip_num = ATOMIC_INIT(0);
u8 fas_ktime_suspended;
s32 fas_ktime_last;
metis_enqueue_entity_fn mi_enqueue_entity_hooks;
unsigned int ui_last_cpu, ren_last_cpu, sf_last_cpu, hwc_last_cpu;
unsigned int foreground_uid;
metis_detect_fn override_schedboost[3];
struct proc_dir_entry *proc_fas_file;
struct metis_detect_slot ui_task_detect[3], render_task_detect[3];
struct metis_vip_detect_slot vip_task_detect[3];
s64 vtask_monitor_beg_us;
u8 vtask_monitor;
metis_lb_fn mi_lb_detect_func[3];
s64 frame_boost_start;
u32 mi_viptask[3];
s64 vtask_monitor_end_us;
s32 last_traced_uid;
unsigned int doublecyc_debug;

/* .data+0x3e80..0x3eaf and .bss+0x15bd0..0x160a7: histories. */
u32 sched_doctor_enable = 1, suspend_vip_enable = 1;
s32 normal_running_too_long_thresh = 3, rt_running_too_long_thresh = 3;
u32 top_task_monitor_window = 1;
struct notifier_block runtime_info_notifier = {
	.notifier_call = kmsg_runtime_info_debug_show,
};
struct metis_run_history his_lru_task[12];
struct metis_final_long_run his_nor_task[10], his_rt_task[10];
struct metis_final_long_run his_max_task[10];
struct proc_dir_entry *runtime_info_entry;
s32 debug_count;
s32 comm_monitor_debug, set_static_vip;
struct workqueue_struct *my_wq;

/* .bss+0x160a8..0x3be0f and .data..read_mostly+0: LB/latency. */
u32 metis_lb_debug, metis_tp_debug;
metis_final_capacity_fn metis_lb_check_for_higher_capacity;
metis_final_overutilized_fn metis_lb_cpu_overutilized;
const struct cpumask *metis_lb_cpu_halt_mask;
struct cpumask ***metis_lb_cpu_array __read_mostly;
s32 metis_lb_min_possible_cluster_id, mi_viptask_balance;
u32 metis_schlat_enable;
u8 metis_schctx_enable;
struct proc_dir_entry *schlat_entry;
struct metis_schlat_ring cpu_sched[32];
struct metis_schlat_long_state long_schlat;
atomic64_t schlat_region_cnt[11];
struct metis_schlat_accumulator schlat_acc;

/* .data+0x3fc8..0x4017 and .bss+0x3be10..0x3bf93: cold start/locks. */
s32 protect_cs = 6, cs_protect_dur = 1000;
u32 cs_stat_type = 15;
u32 coldstart_set_mmlock_holder_vip = 1;
s32 low_lt_preempt_thres = 8000000;
s32 low_lt_preempt_thres_cold_start = 1000000;
u32 set_cold_start_render_vip = 1;
u32 override_schedboost_in_coldstart = 1;
s32 set_possible_notifythr_vip = 1;
u32 rwsem_normal_spin_thres = 15000, rwsem_vip_spin_thres = 40000;
u32 mutex_normal_spin_thres = 60000, mutex_vip_spin_thres = 100000;
u32 read_trylock_reserved_jif = 1, ui_blocked_reader_jif = 2;
u32 reader_biased_jif = 1;
unsigned long rtg_handoff_min_inter = 1;
u32 stask_prefer_cpu = 7;
u32 cold_start_ui_cpu = (u32)-1;
s32 cold_start_sserver_tgid, cold_start_app_tgid;
s32 home_tgid, systemui_tgid, systemserver_tgid, sf_tgid;
void *cold_start_app_mmlock, *cold_start_sserver_mmlock;
void *home_mmlock, *systemui_mmlock, *systemserver_mmlock, *sf_mmlock;
s32 last_ui_forked_pid;
unsigned long cold_start_ui_futex_wait;
u32 reader_biased_jif_for_vip;
unsigned long rtg_rwsem_handoff_jif;
s32 cs_debug;
unsigned long last_ui_forked_jif, coldstart_end_jif, coldstart_begin_jif;
s32 metis_wakeup_enable;
u32 metis_wakeup_debug;
s32 cold_start_ui_state;
char app_wakeup_task[240];

/* .data+0x4018..0x402b and .bss+0x3bf94..0x3bfc7: persist/I/O. */
DEFINE_SPINLOCK(persist_vip_lock);
s32 persist_vip_pids[8];
u32 metis_io_debug;
u32 deadline_enable = 1, metis_io_enabled = 1;
char VERITY_WORK_NAME[12] = "verity_work";
u64 vip_iowait_time;

/* All original .data..percpu+0..0x303 bytes are zero. */
DEFINE_PER_CPU(struct freq_qos_request, metis_req_min);
DEFINE_PER_CPU(struct freq_qos_request, metis_req_max);
DEFINE_PER_CPU_SHARED_ALIGNED(struct metis_rq, metis_runqueues);
DEFINE_PER_CPU(u32, low_lt_num);
/* Original task_of.__already_done, .data..once+0, initial byte00. */
u8 metis_task_of_already_done __section(".data..once");
