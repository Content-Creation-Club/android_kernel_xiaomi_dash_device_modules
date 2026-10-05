// SPDX-License-Identifier: GPL-2.0
/* Stock parameter callback bodies; no parameter registration is installed. */
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/string.h>
#include "migt.h"

/* .text+0xe34..0xf7c. Retain the observed get_pid_task reference leak. */
int set_migt_flag(const char *value, const struct kernel_param *param)
{
	struct pid *pid;
	struct task_struct *task;
	size_t length;
	int number = 0;
	if (sscanf(value, "%d", &number) != 1)
		return -EINVAL;
	pid = find_get_pid(number);
	if (!pid) {
		printk("\0013migt: %s fail to find_get_pid\n", __func__);
		return -EINVAL;
	}
	task = get_pid_task(pid, PIDTYPE_PID);
	length = (long)(int)strlen(param->name);
	if (strnstr(param->name, "add_vip_group", length))
		user_set_game_vip(task, 1);
	if (strnstr(param->name, "add_ip_group", length))
		user_set_game_ip(task, 1);
	if (strnstr(param->name, "clean_user_group", length)) {
		user_set_game_vip(task, 0);
		user_set_game_ip(task, 0);
	}
	put_pid(pid);
	return 0;
}

/* .text+0xf80..0xf88. This empty readback is literal stock behavior. */
int get_migt_flag(char *buffer, const struct kernel_param *param)
{
	return 0;
}

/* .text+0x2b3c..0x2be8. Accepted interval is -1..7, independent of nr_cpu_ids. */
int set_prefer_cpu_with_check(const char *value, const struct kernel_param *param)
{
	int cpu = 0, ret;
	if (sscanf(value, "%d", &cpu) != 1)
		return -EINVAL;
	if ((u32)cpu - 8U <= 0xfffffff6U) {
		printk("\0013migt-gtc: set prefer cpu error, not allowed: %d\n", cpu);
		return -EINVAL;
	}
	ret = param_set_int(value, param);
	if (!ret)
		update_migt_reserve_cpus();
	return ret;
}

/* .text+0x1174..0x1214. Accepted interval is -1..2. */
int set_prefer_cluster_with_check(const char *value, const struct kernel_param *param)
{
	int cluster = 0;
	if (sscanf(value, "%d", &cluster) != 1)
		return -EINVAL;
	if ((u32)cluster - 3U <= 0xfffffffbU) {
		printk("\0013migt: set prefer cluster error, not allowed: %d\n", cluster);
		return -EINVAL;
	}
	return param_set_int(value, param);
}

/* .text+0x546c..0x54c8. One-word backing store and nr_cpu_ids are unchanged. */
int set_migt_reserve_cpus_rt(const char *value, const struct kernel_param *param)
{
	int ret = bitmap_parse(value, (unsigned int)-1,
		(unsigned long *)&migt_reserve_cpus_rt, nr_cpu_ids);
	if (ret)
		printk("\0013migt-gtc: Failed to set cpumask variable migt_reserve_cpus_rt, invalid input: %s\n", value);
	return ret;
}

/* .text+0x54cc..0x5500. */
int get_migt_reserve_cpus_rt(char *buffer, const struct kernel_param *param)
{
	return sprintf(buffer, "%*pbl\n", nr_cpu_ids, &migt_reserve_cpus_rt);
}

/* .text+0x5504..0x5560. */
int set_migt_reserve_cpus_mtk_vip(const char *value, const struct kernel_param *param)
{
	int ret = bitmap_parse(value, (unsigned int)-1,
		(unsigned long *)&migt_reserve_cpus_mtk_vip, nr_cpu_ids);
	if (ret)
		printk("\0013migt-gtc: Failed to set cpumask variable migt_reserve_cpus_mtk_vip, invalid input: %s\n", value);
	return ret;
}

/* .text+0x5564..0x5598. */
int get_migt_reserve_cpus_mtk_vip(char *buffer, const struct kernel_param *param)
{
	return sprintf(buffer, "%*pbl\n", nr_cpu_ids, &migt_reserve_cpus_mtk_vip);
}

static unsigned int next_possible_cpu32(unsigned int first)
{
	u32 mask = (u32)cpumask_bits(cpu_possible_mask)[0];
	mask &= (u32)(~0ULL << (first & 63));
	return mask ? __ffs(mask) : 32;
}

/* .text+0xa58..0xcf0. Partial writes survive malformed later pairs. */
int set_migt_freq(const char *value, const struct kernel_param *param)
{
	const char *cursor = value;
	u32 separators = (u32)-1, cpu = 0, freq = 0, i;
	bool ceiling;
	size_t length = (long)(int)strlen(param->name);
	/* Stock performs this lookup and ignores its result. */
	strnstr(param->name, "migt_freq", length);
	ceiling = strnstr(param->name, "migt_ceiling_freq", length) != NULL;
	do {
		cursor = strpbrk(cursor + 1, " :");
		separators++;
	} while (cursor);
	if (!separators) {
		if (sscanf(value, "%u\n", &freq) != 1)
			return -EINVAL;
		for (cpu = next_possible_cpu32(0); cpu < 32;
		     cpu = next_possible_cpu32(cpu + 1)) {
			if (ceiling)
				per_cpu(migt_info, cpu).configured_max = freq;
			else
				per_cpu(migt_info, cpu).configured_min = freq;
		}
	} else {
		if (!(separators & 1))
			return -EINVAL;
		for (i = 0; (s32)i < (s32)separators; i += 2) {
			if (sscanf(value, "%u:%u", &cpu, &freq) != 2 ||
			    cpu >= hweight32((u32)cpumask_bits(cpu_possible_mask)[0]))
				return -EINVAL;
			BUG_ON(cpu >= 32);
			if (ceiling)
				per_cpu(migt_info, cpu).configured_max = freq;
			else
				per_cpu(migt_info, cpu).configured_min = freq;
			/* Stock advances even after the final pair, including NULL+1. */
			value = (const char *)((unsigned long)strchr(value, ' ') + 1);
		}
	}
	migt_enabled = 0;
	for (cpu = next_possible_cpu32(0); cpu < 32;
	     cpu = next_possible_cpu32(cpu + 1)) {
		if (per_cpu(migt_info, cpu).configured_min) {
			migt_enabled = 1;
			break;
		}
	}
	return 0;
}

/* .text+0xcf4..0xe30. snprintf return lengths accumulate without clamping. */
int get_migt_freq(char *buffer, const struct kernel_param *param)
{
	u32 cpu, freq;
	int used = 0;
	size_t length = (long)(int)strlen(param->name);
	bool ceiling;
	strnstr(param->name, "migt_freq", length);
	ceiling = strnstr(param->name, "migt_ceiling_freq", length) != NULL;
	for (cpu = next_possible_cpu32(0); cpu < 32;
	     cpu = next_possible_cpu32(cpu + 1)) {
		freq = ceiling ? per_cpu(migt_info, cpu).configured_max :
			per_cpu(migt_info, cpu).configured_min;
		used += snprintf(buffer + used, 4096 - (long)used, "%d:%u ", cpu, freq);
	}
	return used + snprintf(buffer + used, 4096 - (long)used, "\n");
}
