// SPDX-License-Identifier: GPL-2.0-only
/* Stock OS3.0.305 GPL export table: 148 original symbols.
 * The three scheduler entries now use shared native-adapted C contracts.
 * Rebuild their provider callers together; original canonical types and
 * stock MODVERSIONS compatibility remain unresolved. No CRC or CFI identity
 * is supplied by hand. Native KMI/CFI/linking are UNTESTED. */
#include <linux/export.h>
#include "metis_common.h"
#include "metis_state.h"
#include "metis_classify.h"
#include "metis_callbacks.h"
#include "metis_registry.h"
#include "metis_coldstart.h"
#include "metis_coldstart_policy.h"
#include "metis_extra_controls.h"
#include "metis_link.h"
#include "metis_lock_wait.h"
#include "metis_vip_queue.h"
#include "metis_cluster_boost.h"
#include "metis_monitor.h"
#include "metis_wakeup.h"
#include "metis_deadline.h"
#include "metis_schlat.h"
#include "metis_io.h"
#include "metis-final-control.h"
#include "metis-final-link.h"
#include "metis-final-scheduler.h"
#include "metis-final-boost.h"
#include "metis-final-owners.h"

EXPORT_SYMBOL_GPL(mi_try_to_wake_up_func);
EXPORT_SYMBOL_GPL(mi_resched_vip_task);
EXPORT_SYMBOL_GPL(mi_link_vip_task);
EXPORT_SYMBOL_GPL(mi_mutex_list_add_hook);
EXPORT_SYMBOL_GPL(mi_art_mutex_vip_task);
EXPORT_SYMBOL_GPL(register_rvh_mi_try_to_wake_up);
EXPORT_SYMBOL_GPL(unregister_rvh_mi_try_to_wake_up);
EXPORT_SYMBOL_GPL(metis_link_init);
EXPORT_SYMBOL_GPL(metis_link_exit);
EXPORT_SYMBOL_GPL(register_game_wake_f);
EXPORT_SYMBOL_GPL(unregister_game_wake_f);
EXPORT_SYMBOL_GPL(mi_select_task_rq_hook);
EXPORT_SYMBOL_GPL(clu_aff_init);
EXPORT_SYMBOL_GPL(clu_aff_exit);
EXPORT_SYMBOL_GPL(in_perf_mod);
EXPORT_SYMBOL_GPL(perf_min_freq);
EXPORT_SYMBOL_GPL(register_corectl_boost_hook);
EXPORT_SYMBOL_GPL(register_update_perflock_freq_hook);
EXPORT_SYMBOL_GPL(register_get_perflock_last_freqs_hook);
EXPORT_SYMBOL_GPL(cpu_to_cluster);
EXPORT_SYMBOL_GPL(trigger_fboost_work);
EXPORT_SYMBOL_GPL(del_cluster);
EXPORT_SYMBOL_GPL(boost_cluster);
EXPORT_SYMBOL_GPL(restore_boost_cluster);
EXPORT_SYMBOL_GPL(store_boost_cluster);
EXPORT_SYMBOL_GPL(get_frmboost_sta_hook);
EXPORT_SYMBOL_GPL(oem_rvh_try_to_wake_up_success);
EXPORT_SYMBOL_GPL(oem_should_update_freq);
EXPORT_SYMBOL_GPL(metis_fboost_init);
EXPORT_SYMBOL_GPL(metis_fboost_exit);
EXPORT_SYMBOL_GPL(choose_cpu_overutiled);
EXPORT_SYMBOL_GPL(show_traced_uid);
EXPORT_SYMBOL_GPL(ui_run_cpu);
EXPORT_SYMBOL_GPL(render_run_cpu);
EXPORT_SYMBOL_GPL(speed_touch_producer_in_boost);
EXPORT_SYMBOL_GPL(speed_touch_consumer_in_boost);
EXPORT_SYMBOL_GPL(mi_walt_get_indicies_func);
EXPORT_SYMBOL_GPL(wake_render_func);
EXPORT_SYMBOL_GPL(mi_enqueue_task_fair_func);
EXPORT_SYMBOL_GPL(mi_dequeue_task_fair_func);
EXPORT_SYMBOL_GPL(get_cpu_util_without);
EXPORT_SYMBOL_GPL(metis_choose_cpu_fastpath);
EXPORT_SYMBOL_GPL(mi_check_preempt_wakeup_hook);
EXPORT_SYMBOL_GPL(metis_coldstart_rt_find_lowest_rq);
EXPORT_SYMBOL_GPL(register_game_migt_skip_cpus_metis_hook);
EXPORT_SYMBOL_GPL(unregister_game_migt_skip_cpus_metis_hook);
EXPORT_SYMBOL_GPL(set_prefer_cluster_with_check);
EXPORT_SYMBOL_GPL(get_mi_fboost_enable);
EXPORT_SYMBOL_GPL(metis_sched_enable);
EXPORT_SYMBOL_GPL(get_mi_link_boost_enable);
EXPORT_SYMBOL_GPL(in_interest_input_event);
EXPORT_SYMBOL_GPL(fas_sched_clock);
EXPORT_SYMBOL_GPL(mi_task_uid);
EXPORT_SYMBOL_GPL(mi_dynamic_vip_task);
EXPORT_SYMBOL_GPL(mi_dynamic_vtask_prio_sched_start);
EXPORT_SYMBOL_GPL(mi_binder_restore_prio_sched_start);
EXPORT_SYMBOL_GPL(mi_set_fas_req);
EXPORT_SYMBOL_GPL(mi_clean_fas_req);
EXPORT_SYMBOL_GPL(register_mi_enqueue_entity_hook);
EXPORT_SYMBOL_GPL(unregister_mi_enqueue_entity_hook);
EXPORT_SYMBOL_GPL(speed_touch_store_freq);
EXPORT_SYMBOL_GPL(speed_touch_restore_freq);
EXPORT_SYMBOL_GPL(mi_enqueue_task_fair_hook);
EXPORT_SYMBOL_GPL(mi_dequeue_task_fair_hook);
EXPORT_SYMBOL_GPL(mi_pick_next_task_fair_hook);
EXPORT_SYMBOL_GPL(get_foreground_uid);
EXPORT_SYMBOL_GPL(set_foreground_app);
EXPORT_SYMBOL_GPL(register_mi_enqueue_task_fair_hook);
EXPORT_SYMBOL_GPL(unregister_mi_enqueue_task_fair_hook);
EXPORT_SYMBOL_GPL(sched_boost_should_disable);
EXPORT_SYMBOL_GPL(mi_ui_task);
EXPORT_SYMBOL_GPL(mi_render_task);
EXPORT_SYMBOL_GPL(mi_vip_task);
EXPORT_SYMBOL_GPL(mi_render_pipeline);
EXPORT_SYMBOL_GPL(metis_vip_task);
EXPORT_SYMBOL_GPL(get_vtask_monitor_status);
EXPORT_SYMBOL_GPL(mi_lb_enable);
EXPORT_SYMBOL_GPL(register_mi_lb_detect_func);
EXPORT_SYMBOL_GPL(unregister_mi_lb_detect_func);
EXPORT_SYMBOL_GPL(register_ui_task_detect);
EXPORT_SYMBOL_GPL(register_render_task_detect);
EXPORT_SYMBOL_GPL(register_vip_task_detect);
EXPORT_SYMBOL_GPL(unregister_ui_task_detect);
EXPORT_SYMBOL_GPL(unregister_render_task_detect);
EXPORT_SYMBOL_GPL(unregister_vip_task_detect);
EXPORT_SYMBOL_GPL(register_mi_walt_get_indicies);
EXPORT_SYMBOL_GPL(unregister_mi_walt_get_indicies);
EXPORT_SYMBOL_GPL(register_mi_find_energy_efficient_cpu);
EXPORT_SYMBOL_GPL(unregister_mi_find_energy_efficient_cpu);
EXPORT_SYMBOL_GPL(register_mi_dequeue_task_fair_hook);
EXPORT_SYMBOL_GPL(unregister_mi_dequeue_task_fair_hook);
EXPORT_SYMBOL_GPL(register_mi_sched_enable);
EXPORT_SYMBOL_GPL(unregister_mi_sched_enable);
EXPORT_SYMBOL_GPL(register_choose_cpu_overutiled);
EXPORT_SYMBOL_GPL(unregister_choose_cpu_overutiled);
EXPORT_SYMBOL_GPL(register_override_schedboost);
EXPORT_SYMBOL_GPL(unregister_override_schedboost);
EXPORT_SYMBOL_GPL(get_metis_limit);
EXPORT_SYMBOL_GPL(get_fboost_duration);
EXPORT_SYMBOL_GPL(mi_after_enqueue_task_hook);
EXPORT_SYMBOL_GPL(mi_after_dequeue_task_hook);
EXPORT_SYMBOL_GPL(metis_runqueues);
EXPORT_SYMBOL_GPL(metis_update_top_task_info);
EXPORT_SYMBOL_GPL(rq_deadloop_detect);
EXPORT_SYMBOL_GPL(mi_scheduler_tick_hook);
EXPORT_SYMBOL_GPL(mi_schedule_hook);
EXPORT_SYMBOL_GPL(metis_doublecyc_init);
EXPORT_SYMBOL_GPL(metis_doublecyc_exit);
EXPORT_SYMBOL_GPL(kernel_set_static_vip);
EXPORT_SYMBOL_GPL(metis_comm_monitor_init);
EXPORT_SYMBOL_GPL(metis_comm_monitor_exit);
EXPORT_SYMBOL_GPL(metis_lb_check_for_higher_capacity);
EXPORT_SYMBOL_GPL(metis_lb_cpu_overutilized);
EXPORT_SYMBOL_GPL(metis_lb_cpu_halt_mask);
EXPORT_SYMBOL_GPL(metis_lb_cpu_array);
EXPORT_SYMBOL_GPL(metis_lb_min_possible_cluster_id);
EXPORT_SYMBOL_GPL(metis_lb_pull_tasks);
EXPORT_SYMBOL_GPL(metis_lb_find_busiest_cpu);
EXPORT_SYMBOL_GPL(metis_lb_init);
EXPORT_SYMBOL_GPL(metis_lb_exit);
EXPORT_SYMBOL_GPL(metis_tp_is_vip_task);
EXPORT_SYMBOL_GPL(metis_tp_mi_viptask_balance);
EXPORT_SYMBOL_GPL(metis_tp_check_cpu_layered_load);
EXPORT_SYMBOL_GPL(metis_tp_fixup_cumulative_runnable_avg_hook);
EXPORT_SYMBOL_GPL(metis_tp_walt_find_best_target_hook);
EXPORT_SYMBOL_GPL(metis_tp_init);
EXPORT_SYMBOL_GPL(metis_tp_exit);
EXPORT_SYMBOL_GPL(metis_schlat_schedule_hook);
EXPORT_SYMBOL_GPL(metis_schlat_dequeue_entity_hook);
EXPORT_SYMBOL_GPL(metis_schlat_init);
EXPORT_SYMBOL_GPL(metis_schlat_exit);
EXPORT_SYMBOL_GPL(mi_wake_up_new_task_hook);
EXPORT_SYMBOL_GPL(in_coldstart);
EXPORT_SYMBOL_GPL(coldstart_app_tasks);
EXPORT_SYMBOL_GPL(coldstart_sserver_tasks);
EXPORT_SYMBOL_GPL(metis_coldstart_skip_thiscpu_rt);
EXPORT_SYMBOL_GPL(metis_coldstart_spread_rt_task);
EXPORT_SYMBOL_GPL(metis_coldstart_skip_lb);
EXPORT_SYMBOL_GPL(metis_wakeup_cold_start_skip_ui_cpumask);
EXPORT_SYMBOL_GPL(metis_wakeup_cold_start_skip_ui_cpu);
EXPORT_SYMBOL_GPL(metis_wakeup_enqueue_task_hook);
EXPORT_SYMBOL_GPL(metis_wakeup_dequeue_task_hook);
EXPORT_SYMBOL_GPL(metis_set_common_persist_vip);
EXPORT_SYMBOL_GPL(metis_common_api_init);
EXPORT_SYMBOL_GPL(metis_common_api_exit);
EXPORT_SYMBOL_GPL(metis_deadline_schedule);
EXPORT_SYMBOL_GPL(metis_deadline_try_to_wake_up);
EXPORT_SYMBOL_GPL(metis_io_schedule_hook);
