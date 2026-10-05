// SPDX-License-Identifier: GPL-2.0
/* Source-only helper lifecycle algorithms; no module entry or state owners. */
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <trace/hooks/cpufreq.h>
#include <trace/hooks/sched.h>
#include "subsystem_lifecycle.h"

/* Repeated stock topology walk extracted from the frame/game init bodies.
 * Only low32 possible CPUs are scanned; no native topology is guessed. */
static unsigned int subsystem_first_cpu(u64 mask)
{
	return mask ? __ffs(mask) : 32;
}

static void subsystem_init_clusters(bool game)
{
	u32 remaining = cpumask_bits(cpu_possible_mask)[0];
	unsigned int start = 0, cpu, member, cluster = 0;
	struct migt_game_cpu *game_cpu;
	cpumask_t group;
	u64 group_bits, scan;

	for (;;) {
		cpu = subsystem_first_cpu((u64)remaining & (~0ULL << start));
		if (cpu >= 32 || cluster > 2)
			break;
		cpumask_clear(&group);
		mi_cpu_coregroup_mask(cpu, &group);
		group_bits = cpumask_bits(&group)[0];
		if (subsystem_first_cpu((u32)group_bits) == cpu) {
			scan = (u32)group_bits;
			while (scan) {
				member = subsystem_first_cpu(scan);
				if (game) {
					game_cpu = &per_cpu(migt_game_load_info, member);
					game_cpu->cluster = cluster;
					/* Raw game init zeroes the actual stock spinlock word. */
					spin_lock_init(&game_cpu->lock);
				} else {
					per_cpu(migt_frame_load_info, member).cluster = cluster;
				}
				scan &= scan - 1;
			}
			BUG_ON(cluster >= 3);
			if (game)
				migt_game_cluster_cpumask[cluster] = group_bits;
			else
				migt_frame_cluster_cpumask[cluster] = group_bits;
			remaining &= ~(u32)group_bits;
			cluster++;
		}
		if (cpu == 31)
			break;
		start = cpu + 1;
	}
	if (game)
		migt_game_cluster_number = cluster;
	else
		migt_frame_cluster_number = cluster;
}

/* .text+0x33d0..0x35b8. Native lock init replaces raw seq/lock zeroing. */
int migt_sched_init(void)
{
	struct task_struct *thread;

	thread = kthread_create_on_node(migt_hardwork, &g_work, -1, "migt_thread");
	/* Stock wakes a non-error pointer BEFORE the NULL test and lock setup. */
	if (!IS_ERR(thread))
		wake_up_process(thread);
	migt_thread = thread;
	if (IS_ERR_OR_NULL(thread)) {
		printk("\0013migt-gtc: migt_thread %s: error create thread!\n", __func__);
		return -1;
	}
	seqlock_init(&render_info[0].lock);
	reset_render_info(0);
	seqlock_init(&render_info[1].lock);
	reset_render_info(1);
	register_vip_task_detect(0, game_vvip_task);
	register_ui_task_detect(0, game_ssuper_task, game_stask_pipeline);
	register_render_task_detect(0, game_render_task, game_render_pipeline);
	register_mi_lb_detect_func(0, game_lb_control);
	register_choose_cpu_overutiled(0, game_choose_cpu_overutiled);
	register_game_migt_select_cpu_hook(migt_choose_exclusive_cpu);
	register_game_migt_skip_cpus_hook(migt_skip_cpus);
	register_game_migt_skip_cpus_metis_hook(migt_skip_cpus);
	register_game_migt_should_skip_this_cpu_rt_hook(migt_should_skip_this_cpu_rt);
	register_game_migt_should_skip_migrate_hook(migt_should_skip_migrate);
	register_game_migt_skip_sched_tick_migrate_hook(migt_should_skip_migrate);
	register_game_migt_should_skip_newidle_balance_hook(migt_should_skip_this_cpu);
	register_game_migt_skip_lowest_rt_hook(migt_skip_lowest_rt);
	register_game_migt_skip_vip_cpus_hook(migt_skip_mtk_vip_cpus);
	register_game_migt_should_skip_affinity_task_hook(migt_should_skip_mtk_affinity_task);
	register_game_migt_group_dep_hook(migt_add_task_to_second);
	update_migt_reserve_cpus();
	printk("\0016migt-gtc: migt_sched %s: inited!\n", __func__);
	migt_init_sucess = 1;
	BUG_ON(migt_sched_header != NULL);
	migt_sched_header = register_sysctl_sz("migt", migt_table, 7);
	return 0;
}

/* .text+0x5124..0x51cc. The omitted UI/render unregisters are stock behavior. */
void migt_sched_exit(void)
{
	migt_init_sucess = 0;
	unregister_mi_lb_detect_func(0);
	unregister_vip_task_detect(0);
	unregister_choose_cpu_overutiled(0);
	unregister_game_migt_select_cpu_hook();
	unregister_game_migt_skip_cpus_hook();
	unregister_game_migt_skip_cpus_metis_hook();
	unregister_game_migt_should_skip_migrate_hook();
	unregister_game_migt_should_skip_this_cpu_rt_hook();
	unregister_game_migt_skip_sched_tick_migrate_hook();
	unregister_game_migt_should_skip_newidle_balance_hook();
	unregister_game_migt_skip_lowest_rt_hook();
	unregister_game_migt_skip_vip_cpus_hook();
	unregister_game_migt_should_skip_affinity_task_hook();
	unregister_game_migt_group_dep_hook();
	unregister_sysctl_table(migt_sched_header);
	migt_sched_header = NULL;
	if (!IS_ERR_OR_NULL(migt_thread))
		kthread_stop(migt_thread);
	printk("\0016migt-gtc: %s, success \n", __func__);
}

/* .text+0x5ba0..0x5de0. All registration/proc errors are ignored by stock. */
int frame_load_init(struct proc_dir_entry *parent)
{
	need_to_base = 0;
	need_to_update = 0;
	need_to_record = 0;
	flw_work_in_process = 0;
	subsystem_init_clusters(false);
	register_game_wake_f(migt_render_link_flw);
	register_mi_enqueue_task_fair_hook(0, game_set_cid);
	register_trace_android_rvh_cpufreq_transition(android_rvh_cpufreq_transition, NULL);
	if (parent) {
		frame_load_dir = proc_mkdir("flw", parent);
		migt_frame_history_entry = proc_create("frame_history_load", 0664,
				frame_load_dir, &migt_frame_history_proc_ops);
		migt_frame_cpu_entry = proc_create("cpu_history_load", 0664,
				frame_load_dir, &migt_frame_cpu_proc_ops);
	}
	frame_load_has_init = 1;
	printk("\0013flw: frame load init success\n");
	return 0;
}

/* .text+0x6038..0x60ac. No proc-pointer reset or restricted-hook removal. */
void frame_load_exit(void)
{
	frame_load_has_init = 0;
	if (frame_load_dir) {
		if (migt_frame_history_entry)
			proc_remove(migt_frame_history_entry);
		if (migt_frame_cpu_entry)
			proc_remove(migt_frame_cpu_entry);
		proc_remove(frame_load_dir);
	}
	unregister_mi_enqueue_task_fair_hook(0);
	unregister_game_wake_f();
	irq_work_sync(&flw_cpufreq_irq_work);
}

/* .text+0x6d24..0x6ef4. Does not register android_vh_map_util_freq. */
int game_load_init(void)
{
	subsystem_init_clusters(true);
	BUG_ON(game_load_header != NULL);
	game_load_header = register_sysctl_sz("migt/glk", game_load_table, 11);
	register_oem_freq_hook(set_next_freq);
	game_load_has_init = 1;
	printk("\0013glk: game load init success\n");
	return 0;
}

/* .text+0x7370..0x73d4. Retain the unmatched normal-trace unregister. */
void game_load_exit(void)
{
	game_load_has_init = 0;
	unregister_trace_android_vh_map_util_freq(set_next_freq, NULL);
	unregister_oem_freq_hook();
	if (game_load_header)
		unregister_sysctl_table(game_load_header);
	game_load_header = NULL;
	printk("\0016glk: game load exit success\n");
}

/* .text+0x6210..0x6234. Shared single wake slot gets the other callback. */
void game_link_init(void)
{
	register_game_wake_f(migt_frame_link_flw);
}

/* .text+0x62a0..0x62bc. No prior callback restoration or owner check. */
void game_link_exit(void)
{
	unregister_game_wake_f();
}
