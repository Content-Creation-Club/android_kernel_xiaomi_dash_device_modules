// SPDX-License-Identifier: GPL-2.0
/* Stock entry algorithms. Native build and runtime remain untested. */
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/printk.h>
#include "module_lifecycle.h"
#include "owners.h"

/* Source extraction of the repeated stock unwind, not another stock body. */
static void migt_remove_freq_qos_requests(void)
{
	struct freq_qos_request *request;
	unsigned int cpu;

	for_each_cpu(cpu, cpu_present_mask) {
		if (cpu >= 32)
			break;
		request = &per_cpu(qos_req_min, cpu);
		if (request && freq_qos_request_active(request))
			freq_qos_remove_request(request);
		request = &per_cpu(qos_req_max, cpu);
		if (request && freq_qos_request_active(request))
			freq_qos_remove_request(request);
	}
}

/* Source extraction of the stock proc cleanup, not another stock body. */
static void migt_remove_proc_entries(void)
{
	if (migt_dir) {
		if (exec_buckets_entry)
			proc_remove(exec_buckets_entry);
		delete_game_load_proc();
		proc_remove(migt_dir);
	}
}

/* Stock init_module .text+0x4fc..0xa54, entry CFI 36b1c5a6: int(void). */
int migt_init(void)
{
	struct cpufreq_policy *policy;
	unsigned int cpu;
	int ret;

	migt_sched_init();
	game_load_init();
	cpus_visit = cpumask_bits(cpu_possible_mask)[0];
	for_each_cpu(cpu, cpu_present_mask) {
		if (cpu >= 32)
			break;
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			printk("\0013migt: %s: Failed to get cpufreq policy for cpu%d\n",
				"migt_freq_qos_request_init_first", cpu);
			goto initial_qos_failed;
		}
		ret = freq_qos_add_request(&policy->constraints,
			&per_cpu(qos_req_min, cpu), FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT);
		if (ret < 0) {
			printk("\0013migt: %s: Failed to add min freq constraint (%d)\n",
				"migt_freq_qos_request_init_first", ret);
			cpufreq_cpu_put(policy);
			goto initial_qos_failed;
		}
		ret = freq_qos_add_request(&policy->constraints,
			&per_cpu(qos_req_max, cpu), FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT);
		if (ret < 0) {
			printk("\0013migt: %s: Failed to add max freq constraint (%d)\n",
				"migt_freq_qos_request_init_first", ret);
			cpufreq_cpu_put(policy);
			goto initial_qos_failed;
		}
		cpufreq_cpu_put(policy);
	}
	goto init_timers;

initial_qos_failed:
	migt_remove_freq_qos_requests();
	printk("\0013migt: migt_freq failed %d to wq!\n", 1444);
	ret = cpufreq_register_notifier(&init_freq_qos_notifier,
		CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		goto unregister_notifier;

init_timers:
	hrtimer_init(&migt_frame_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	migt_frame_timer.function = do_boost_work;
	hrtimer_init(&vtask_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vtask_timer.function = do_boost_work;
	migt_wq = alloc_workqueue("migt_wq", WQ_HIGHPRI, 0);
	if (!migt_wq) {
		ret = -EFAULT;
		goto remove_qos;
	}
	migt_dir = proc_mkdir("migt", NULL);
	if (!migt_dir) {
		ret = -EFAULT;
		goto destroy_queue;
	}
	exec_buckets_entry = proc_create("show_exec_buckets", 0664, migt_dir,
		&migt_exec_buckets_proc_ops);
	create_game_load_proc(migt_dir);
	ret = misc_register(&migt_misc);
	if (ret) {
		migt_remove_proc_entries();
		goto destroy_queue;
	}
	/* The stock misc device becomes visible before its work initialization. */
	INIT_DELAYED_WORK(&migt_work, do_migt);
	INIT_WORK(&irq_boost_work, do_migt);
	INIT_DELAYED_WORK(&migt_rem, do_migt_rem);
	INIT_DELAYED_WORK(&ceiling_work, do_ceiling_limit);
	INIT_DELAYED_WORK(&ceiling_rem, do_ceiling_rem);
	spin_lock_init(&migt_lock);
	for_each_cpu(cpu, cpu_possible_mask) {
		if (cpu >= 32)
			break;
		per_cpu(migt_info, cpu).unknown_00 = cpu;
		per_cpu(migt_info, cpu).max_freq = FREQ_QOS_MAX_DEFAULT;
	}
	migt_sched_enable = migt_enable;
	register_mi_sched_enable(0, migt_enable);
	register_sched_load_update_hook(0, migt_hook);
	register_mi_find_energy_efficient_cpu(0, find_energy_hook);
	register_mi_walt_get_indicies(0, walt_get_indicies_hook);
	return 0;

destroy_queue:
	flush_workqueue(migt_wq);
	destroy_workqueue(migt_wq);
remove_qos:
	migt_remove_freq_qos_requests();
unregister_notifier:
	cpufreq_unregister_notifier(&init_freq_qos_notifier,
		CPUFREQ_POLICY_NOTIFIER);
	game_load_exit();
	migt_sched_exit();
	return ret;
}

/* Stock cleanup_module .exit.text+0x4..0x160, CFI a540670c: void(void). */
void migt_exit(void)
{
	frame_load_exit();
	migt_sched_enable = NULL;
	unregister_sched_load_update_hook(0);
	unregister_mi_find_energy_efficient_cpu(0);
	unregister_mi_walt_get_indicies(0);
	unregister_mi_sched_enable(0);
	misc_deregister(&migt_misc);
	migt_remove_proc_entries();
	flush_workqueue(migt_wq);
	destroy_workqueue(migt_wq);
	migt_remove_freq_qos_requests();
	game_load_exit();
	migt_sched_exit();
	printk("%s success\n", "migt_exit");
}

module_init(migt_init);
module_exit(migt_exit);
