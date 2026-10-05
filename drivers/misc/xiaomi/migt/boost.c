// SPDX-License-Identifier: GPL-2.0
/* Real stock boost/QoS consumers. Workqueue/timer lifecycle remains absent. */
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include "migt.h"

DEFINE_PER_CPU(struct migt_cpu_info, migt_info);
DEFINE_PER_CPU(struct freq_qos_request, qos_req_min);
DEFINE_PER_CPU(struct freq_qos_request, qos_req_max);
int migt_debug, migt_ms = 50, cpu_boost_cycle = 10, boost_cycle;
int vip_task_schedboost, ceiling_from_user;
u8 migt_enabled, ceiling_freq_limit_work;
u32 ceiling_users[10];
int ceiling_users_count;
extern int core_ctl_set_boost(bool boost);

/* Consume only the original low32 mask; CPU32 is the stock end sentinel. */
static unsigned int next_cpu32(const struct cpumask *mask, unsigned int first)
{
	u32 bits = (u32)cpumask_bits(mask)[0];
	bits &= (u32)(~0ULL << (first & 63));
	return bits ? __ffs(bits) : 32;
}

/* .text+0x1218..0x1258. One-shot hrtimer callback, returns HRTIMER_NORESTART. */
enum hrtimer_restart do_boost_work(struct hrtimer *timer)
{
	if (!work_pending(&irq_boost_work))
		queue_work_on(WORK_CPU_UNBOUND, migt_wq, &irq_boost_work);
	return HRTIMER_NORESTART;
}

/* .text+0x2738..0x29d8. Select lowest online CPU per topology cluster. */
unsigned int update_policy_online(unsigned int force)
{
	struct cpufreq_policy *policy;
	struct cpumask cluster_mask;
	unsigned int cpu, first_online, clusters = 0, failed = 0;
	u64 online;
	u32 min_freq, max_freq;
	int ret;

	cpus_read_lock();
	for (cpu = next_cpu32(cpu_possible_mask, 0); cpu < 32;
	     cpu = next_cpu32(cpu_possible_mask, cpu + 1)) {
		if (!cpu_online(cpu))
			failed = 1;
		cpumask_clear(&cluster_mask);
		mi_cpu_coregroup_mask(cpu, &cluster_mask);
		online = (u32)cpumask_bits(&cluster_mask)[0] &
			cpumask_bits(cpu_online_mask)[0];
		first_online = online ? __ffs64(online) : 32;
		if (first_online != cpu)
			continue;
		if (!((((u32)boost_policy - 2U < 2U) && clusters != 0) ||
		      (boost_policy == 1 && clusters == 1) || (force & 1))) {
			clusters++;
			continue;
		}
		min_freq = per_cpu(migt_info, cpu).min_freq;
		max_freq = per_cpu(migt_info, cpu).max_freq;
		policy = cpufreq_cpu_get(cpu);
		if (policy) {
			BUG_ON(policy->cpu >= 32);
			ret = freq_qos_update_request(&per_cpu(qos_req_min, policy->cpu),
				min_freq);
			if (ret >= 0) {
				BUG_ON(policy->cpu >= 32);
				ret = freq_qos_update_request(&per_cpu(qos_req_max, policy->cpu),
					max_freq);
			}
			cpufreq_cpu_put(policy);
			if (ret < 0) {
				failed = 1;
				break;
			}
		}
		clusters++;
	}
	cpus_read_unlock();
	/* Actual ORN followed by AND1; force0 returns1 even without a failure. */
	return (failed | ~force) & 1;
}

/* .text+0x125c..0x1398. Cancelled removal suppresses an extra core-boost ref. */
void do_migt(struct work_struct *work)
{
	unsigned int cpu;
	bool cancelled = cancel_delayed_work_sync(&migt_rem);
	if (migt_debug)
		printk("\0013migt: migt boost start....%d %llu", 585,
			(u64)(ktime_to_ns(ktime_get()) / 1000));
	vip_task_schedboost = 1;
	if (!cancelled)
		core_ctl_set_boost(true);
	glk_maxfreq_break(1);
	for (cpu = next_cpu32(cpu_possible_mask, 0); cpu < 32;
	     cpu = next_cpu32(cpu_possible_mask, cpu + 1))
		per_cpu(migt_info, cpu).min_freq = per_cpu(migt_info, cpu).configured_min;
	if (!affinity_only)
		update_policy_online(0);
	queue_delayed_work_on(WORK_CPU_UNBOUND, migt_wq, &migt_rem,
		msecs_to_jiffies((u32)migt_ms));
	boost_cycle = cpu_boost_cycle;
}

/* .text+0x139c..0x1558. Recovery delay3750 is already jiffies, not ms. */
void do_migt_rem(struct work_struct *work)
{
	unsigned int cpu, failed;
	boost_cycle = (u32)boost_cycle - 1;
	if (boost_cycle >= 1) {
		queue_delayed_work_on(WORK_CPU_UNBOUND, migt_wq, &migt_rem,
			msecs_to_jiffies((u32)migt_ms));
		glk_force_maxfreq_break(1);
		return;
	}
	glk_force_maxfreq_break(0);
	if (migt_debug)
		printk("\0013migt: migt boost end...%d %llu", 535,
			(u64)(ktime_to_ns(ktime_get()) / 1000));
	glk_maxfreq_break(0);
	for (cpu = next_cpu32(cpu_possible_mask, 0); cpu < 32;
	     cpu = next_cpu32(cpu_possible_mask, cpu + 1))
		per_cpu(migt_info, cpu).min_freq = 0;
	failed = update_policy_online(1);
	core_ctl_set_boost(false);
	if (failed) {
		for (cpu = next_cpu32(cpu_possible_mask, 0); cpu < 32;
		     cpu = next_cpu32(cpu_possible_mask, cpu + 1)) {
			if (!per_cpu(qos_req_min, cpu).pnode.prio) {
				core_ctl_set_boost(true);
				queue_delayed_work_on(WORK_CPU_UNBOUND, migt_wq,
					&migt_rem, 3750);
				break;
			}
		}
	}
	vip_task_schedboost = 0;
}

/* .text+0x155c..0x1758. Admission flag is set even without ceiling_from_user. */
void do_ceiling_limit(struct work_struct *work)
{
	unsigned int cpu;
	int i, count;
	if (!(ceiling_freq_limit_work & 1)) {
		count = ceiling_users_count;
		if (count < 1)
			return;
		count = min(count, 10);
		for (i = 0; i < count; ++i)
			if (ceiling_users[i] == last_traced_uid)
				break;
		if (i == count)
			return;
		ceiling_freq_limit_work = 1;
	}
	if (migt_debug)
		printk("\0013migt: migt trigger_ceiling_limit ....%d %llu\n", 777,
			(u64)(ktime_to_ns(ktime_get()) / 1000));
	if (!ceiling_from_user)
		return;
	for (cpu = next_cpu32(cpu_possible_mask, 0); cpu < 32;
	     cpu = next_cpu32(cpu_possible_mask, cpu + 1))
		per_cpu(migt_info, cpu).max_freq = per_cpu(migt_info, cpu).configured_max;
	if (!affinity_only)
		update_policy_online(0);
}

/* .text+0x175c..0x1838. Exact byte equality1, not a Boolean/nonzero gate. */
void do_ceiling_rem(struct work_struct *work)
{
	unsigned int cpu;
	if (ceiling_freq_limit_work != 1)
		return;
	if (migt_debug == 1)
		printk("\0013migt: migt do ceiling_rem....%d %llu", 704,
			(u64)(ktime_to_ns(ktime_get()) / 1000));
	for (cpu = next_cpu32(cpu_possible_mask, 0); cpu < 32;
	     cpu = next_cpu32(cpu_possible_mask, cpu + 1))
		per_cpu(migt_info, cpu).max_freq = 0x7fffffff;
	update_policy_online(1);
	ceiling_freq_limit_work = 0;
}

/* Stock .data+0x50 notifier and .data+0xb8 preinitialized work. They are
 * locally described but never registered or queued by a missing startup. */
static struct notifier_block init_freq_qos_notifier = {
	.notifier_call = migt_freq_qos_init,
};
static DECLARE_WORK(finish_freq_qos_work, finish_freq_qos_workfn);

/* .text+0x19f8..0x1bfc. Ignores the event's policy pointer, visits every CPU. */
int migt_freq_qos_init(struct notifier_block *nb, unsigned long event, void *data)
{
	struct cpufreq_policy *policy;
	unsigned int cpu;
	int ret;
	if (event != CPUFREQ_CREATE_POLICY)
		return 0;
	for (cpu = next_cpu32(cpu_present_mask, 0); cpu < 32;
	     cpu = next_cpu32(cpu_present_mask, cpu + 1)) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			printk("\0013migt: %s: Failed to get cpufreq policy for cpu%d\n",
				__func__, cpu);
			ret = -EAGAIN;
			goto remove_requests;
		}
		cpus_visit = (u32)cpus_visit & ~(u32)cpumask_bits(policy->related_cpus)[0];
		ret = freq_qos_add_request(&policy->constraints, &per_cpu(qos_req_min, cpu),
			FREQ_QOS_MIN, 0);
		if (ret < 0) {
			printk("\0013migt: %s: Failed to add min freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto remove_requests;
		}
		ret = freq_qos_add_request(&policy->constraints, &per_cpu(qos_req_max, cpu),
			FREQ_QOS_MAX, 0x7fffffff);
		if (ret < 0) {
			printk("\0013migt: %s: Failed to add max freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto remove_requests;
		}
		cpufreq_cpu_put(policy);
	}
	if (!(u32)cpus_visit)
		queue_work_on(WORK_CPU_UNBOUND, system_wq, &finish_freq_qos_work);
	return 0;

remove_requests:
	for (cpu = next_cpu32(cpu_present_mask, 0); cpu < 32;
	     cpu = next_cpu32(cpu_present_mask, cpu + 1)) {
		if (freq_qos_request_active(&per_cpu(qos_req_min, cpu)))
			freq_qos_remove_request(&per_cpu(qos_req_min, cpu));
		if (freq_qos_request_active(&per_cpu(qos_req_max, cpu)))
			freq_qos_remove_request(&per_cpu(qos_req_max, cpu));
	}
	return ret;
}

/* .text+0x1c00..0x1c34. */
void finish_freq_qos_workfn(struct work_struct *work)
{
	cpufreq_unregister_notifier(&init_freq_qos_notifier, CPUFREQ_POLICY_NOTIFIER);
	printk("\0013migt: finish_freq_qos_workfn!");
}
