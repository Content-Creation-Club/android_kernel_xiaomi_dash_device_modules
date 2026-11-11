// SPDX-License-Identifier: GPL-2.0-only
/* Register the core's resident WLUN driver when LK blacklists ufshcd_core_init. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <scsi/scsi_driver.h>

static int __init __nocfi wlun_init(void)
{
	unsigned long (*lookup_name)(const char *name);
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	struct scsi_driver *wlun;
	struct dentry **debugfs_root;
	int ret;

	ret = register_kprobe(&kp);
	if (ret) {
		pr_err("cannot resolve kallsyms_lookup_name: %d\n", ret);
		return ret;
	}
	lookup_name = (void *)kp.addr;
	unregister_kprobe(&kp);

	/*
	 * These are explicit private-symbol dependencies, not stable KMI.
	 * Do not call ufshcd_core_init: its __init text has already been freed.
	 * The resident template retains the core's original probe and PM callbacks.
	 */
	wlun = (void *)lookup_name("ufs_dev_wlun_template");
	if (!wlun) {
		pr_err("core has no resident ufs_dev_wlun_template\n");
		return -ENOENT;
	}
	if (READ_ONCE(wlun->gendrv.p)) {
		pr_info("core WLUN driver is already registered\n");
		return 0;
	}

	debugfs_root = (void *)lookup_name("ufs_debugfs_root");
	if (!debugfs_root) {
		pr_err("core has no resident ufs_debugfs_root\n");
		return -ENOENT;
	}
	*debugfs_root = debugfs_create_dir("ufshcd", NULL);
	ret = scsi_register_driver(&wlun->gendrv);
	if (ret) {
		debugfs_remove(*debugfs_root);
		*debugfs_root = NULL;
		pr_err("cannot register core WLUN driver: %d\n", ret);
		return ret;
	}
	pr_info("registered core WLUN driver with original PM callbacks\n");
	return 0;
}
module_init(wlun_init);

/* Keep the storage bootstrap resident; unloading it is not a supported path. */
MODULE_DESCRIPTION("UFS WLUN bootstrap for the bootloader initcall blacklist");
MODULE_LICENSE("GPL");
