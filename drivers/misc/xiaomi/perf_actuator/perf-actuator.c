// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 GitHub @YorokobiMaster
 *
 * Reimplemented from the observable ABI and behavior of Xiaomi's stock
 * perf-actuator module for dash.
 */

#define pr_fmt(fmt) "perf_actuator: " fmt

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/rwsem.h>

#include "perf_actuator.h"

#define PERF_ACTUATOR_MAGIC	'x'
#define PERF_ACTUATOR_COUNT	37

struct perf_actuator_entry {
	unsigned int cmd;
	perf_actuator_callback_t callback;
};

/*
 * Intentional source-only stock difference: the stock table has a global ELF
 * name but is not exported. Keep it private; this does not change module ABI.
 */
static struct perf_actuator_entry perf_actuator_table[PERF_ACTUATOR_COUNT];
static DECLARE_RWSEM(table_rwsem);

int register_perf_actuator(unsigned int cmd,
			   perf_actuator_callback_t callback)
{
	unsigned int index = _IOC_NR(cmd);

	if (index >= ARRAY_SIZE(perf_actuator_table) || !callback)
		return -EINVAL;

	down_write(&table_rwsem);
	if (perf_actuator_table[index].callback)
		pr_err("perf_actuator: register %u conflict\n", cmd);
	perf_actuator_table[index].cmd = cmd;
	perf_actuator_table[index].callback = callback;
	up_write(&table_rwsem);

	return 0;
}
EXPORT_SYMBOL_GPL(register_perf_actuator);

int unregister_perf_actuator(unsigned int cmd)
{
	unsigned int index = _IOC_NR(cmd);

	if (index >= ARRAY_SIZE(perf_actuator_table))
		return -EINVAL;

	down_write(&table_rwsem);
	if (!perf_actuator_table[index].callback ||
	    perf_actuator_table[index].cmd != cmd)
		pr_err("perf_actuator: unregister %u conflict\n", cmd);
	perf_actuator_table[index].cmd = 0;
	perf_actuator_table[index].callback = NULL;
	up_write(&table_rwsem);

	return 0;
}
EXPORT_SYMBOL_GPL(unregister_perf_actuator);

static long perf_actuator_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct perf_actuator_entry *entry;
	unsigned int index;
	long ret;

	if (!arg) {
		pr_err("perf_actuator: invalid user uarg\n");
		return -EINVAL;
	}
	if (_IOC_TYPE(cmd) != PERF_ACTUATOR_MAGIC) {
		pr_err("perf_actuator: invalid magic %u\n", _IOC_TYPE(cmd));
		return -EINVAL;
	}

	index = _IOC_NR(cmd);
	if (index >= ARRAY_SIZE(perf_actuator_table)) {
		pr_err("perf_actuator: invalid cmd, %u > %d\n", index,
		       PERF_ACTUATOR_COUNT);
		return -ERANGE;
	}

	down_read(&table_rwsem);
	entry = &perf_actuator_table[index];
	if (entry->callback && entry->cmd == cmd) {
		ret = entry->callback((void *)arg);
	} else {
		pr_err("perf_actuator: idx %d, cmd %d not match or not registered!\n",
		       (int)index, (int)cmd);
		ret = -ENOENT;
	}
	up_read(&table_rwsem);

	return ret;
}

static int perf_actuator_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int perf_actuator_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations perf_actuator_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = perf_actuator_ioctl,
	.open = perf_actuator_open,
	.release = perf_actuator_release,
};

static struct miscdevice perf_actuator_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xr_perf_actuator",
	.fops = &perf_actuator_fops,
};

static int __init perf_actuator_init(void)
{
	int ret;

	ret = misc_register(&perf_actuator_device);
	if (ret)
		pr_err("register perf_actuator_device failed, err:%d", ret);

	return ret;
}

static void __exit perf_actuator_exit(void)
{
	misc_deregister(&perf_actuator_device);
}

module_init(perf_actuator_init);
module_exit(perf_actuator_exit);

MODULE_LICENSE("GPL v2");
