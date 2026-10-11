// SPDX-License-Identifier: GPL-2.0-only
/* OS3.0.305 stock lifecycle. Original missing cleanup and ignored errors
 * are intentionally retained. Native linkage and operation are UNTESTED. */
#include <linux/cpumask.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/string.h>
#include <trace/hooks/binder.h>
#include <trace/hooks/sched.h>
#include "metis-final-module.h"
#include "metis-final-native.h"
#include "metis-final-link.h"
#include "metis-final-control.h"
#include "metis-final-scheduler.h"
#include "metis-final-boost.h"
#include "metis_registry.h"
#include "metis_io.h"

/* Stock .text+dcdc..dce0 is genuinely one RET, with no state effect. */
void set_foreground_pid(unsigned int pid) { }

/* Stock .text+ff04..10154. These offsets belong to Xiaomi's private
 * mi_rq/metis_rq owners, never to native struct rq. */
int fas_init(void)
{
	u32 pending = (u32)cpu_possible_mask->bits[0];
	unsigned int cpu;
	struct mi_rq *mi;
	struct metis_rq *state;

	while (pending) {
		cpu = __ffs(pending);
		pending &= pending - 1;
		mi = per_cpu_ptr(&mi_runqueues, cpu);
		memset(mi->raw, 0, 0x150);
		MI_FIELD(mi, u64, 0x150) = 0;
		MI_FIELD(mi, u64, 0x158) = 0;
		MI_FIELD(mi, struct rb_root_cached, 0x160) = RB_ROOT_CACHED;
		MI_FIELD(mi, u32, 0x1c8) = 0;
		INIT_LIST_HEAD(&MI_FIELD(mi, struct list_head, 0x1d0));
		state = per_cpu_ptr(&metis_runqueues, cpu);
		/* Stock clears only these fields, leaving gaps intact. */
		memset(state->raw + 0x98, 0, 0x170);
		memset(state->raw + 0x208, 0, 0x60);
		MI_FIELD(state, u64, 0x270) = 0;
		MI_FIELD(state, u8, 0x268) = 1;
		MI_FIELD(state, u64, 0x278) = 0;
		MI_FIELD(state, u64, 0x08) = 0;
		MI_FIELD(state, u64, 0x10) = 0;
		MI_FIELD(state, u8, 0x18) = 1;
		MI_FIELD(state, u64, 0x00) = 0;
		/* Original STUR clears the unaligned private bytes +1c..+23. */
		memset(state->raw + 0x1c, 0, 8);
		MI_FIELD(state, u32, 0x70) = 0;
		MI_FIELD(state, u32, 0x48) = 0;
		memset(state->raw + 0x28, 0, 0x20);
	}
	proc_fas_file = proc_create("fas", 0644, NULL, &proc_fas_op);
	if (!proc_fas_file)
		printk("\0013metis-fas: [ST-SCHED-TF-METIS]proc_fas file create failed.\n");
	register_trace_android_rvh_enqueue_entity(mi_rb_enqueue_entity_hook, NULL);
	register_trace_android_rvh_dequeue_entity(mi_rb_dequeue_entity_hook, NULL);
	register_trace_android_rvh_entity_tick(mi_check_preempt_tick_hook, NULL);
	register_mi_enqueue_task_fair_hook(1, before_enqueue_vip_task);
	register_syscore_ops(&fas_syscore_ops);
	printk("\0013metis-fas: FAS: %s: inited!\n", "fas_init");
	return 0;
}

/* Stock .text+1033c..103a0: no entity-hook unregister or pointer reset. */
void fas_exit(void)
{
	if (proc_fas_file)
		proc_remove(proc_fas_file);
	unregister_mi_enqueue_task_fair_hook(1);
	unregister_syscore_ops(&fas_syscore_ops);
	printk("\0013metis-fas: FAS: %s: exit!\n", "fas_exit");
}

/* Stock init_module .init.text+4..258. Only misc_register determines
 * the returned status; earlier registration/init failures have no unwind. */
static int __init metis_module_init(void)
{
	int ret;

	register_mi_dequeue_task_fair_hook(1, mi_dequeue_task_fair_hook);
	register_mi_find_energy_efficient_cpu(1, find_energy_hook);
	register_mi_walt_get_indicies(1, walt_get_indicies_hook);
	register_trace_android_rvh_after_enqueue_task(mi_after_enqueue_task_hook, NULL);
	register_vip_task_detect(1, metis_vvip_task);
	register_mi_sched_enable(1, get_mi_fboost_enable);
	register_ui_task_detect(1, metis_ui_task_quick, metis_ui_pipeline);
	register_render_task_detect(1, metis_ren_task, metis_render_pipeline);
	register_override_schedboost(1, coldstart_override_schedboost);
	register_mi_lb_detect_func(1, metis_coldstart_skip_lb);
	register_trace_android_vh_binder_alloc_new_buf_locked(
		android_vh_binder_alloc_new_buf_locked, NULL);
	metis_link_init();
	metis_doublecyc_init();
	printk("\0016metis-fas: metis_init, after metis_doublecyc_init!\n");
	clu_aff_init();
	printk("\0016metis-fas: metis_init, after clu_aff_init!\n");
	metis_fboost_init();
	fas_init();
	metis_comm_monitor_init();
	metis_io_init();
	metis_deadline_init();
	metis_lb_init();
	metis_tp_init();
	metis_schlat_init();
	metis_cs_init();
	metis_wakeup_init();
	metis_common_api_init();
	ret = misc_register(&metis_misc);
	frame_boost_start = ktime_to_us(ktime_get());
	printk("\0016metis-fas: metis init!\n");
	return ret;
}

/* Stock cleanup_module .exit.text+4..170, including asymmetric teardown. */
static void __exit metis_module_exit(void)
{
	metis_cs_exit();
	metis_link_exit();
	clu_aff_exit();
	metis_doublecyc_exit();
	metis_fboost_exit();
	fas_exit();
	metis_lb_exit();
	metis_tp_exit();
	metis_schlat_exit();
	metis_wakeup_exit();
	metis_common_api_exit();
	unregister_override_schedboost(1);
	unregister_mi_lb_detect_func(1);
	unregister_mi_sched_enable(1);
	unregister_ui_task_detect(1);
	unregister_vip_task_detect(1);
	unregister_mi_enqueue_task_fair_hook(1);
	unregister_mi_dequeue_task_fair_hook(1);
	unregister_mi_find_energy_efficient_cpu(1);
	unregister_mi_walt_get_indicies(1);
	metis_comm_monitor_exit();
	metis_io_exit();
	misc_deregister(&metis_misc);
}
module_init(metis_module_init);
module_exit(metis_module_exit);
MODULE_LICENSE("GPL");
