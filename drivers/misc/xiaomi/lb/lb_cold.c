// SPDX-License-Identifier: GPL-2.0-only
/* dash OS3.0.305 lb.ko reconstruction. Evidence ranges are stock .text offsets. */
#include <linux/bitmap.h>
#include <linux/cred.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <trace/hooks/mm.h>
#include "lb.h"

int g_lb_msg_level = 3;
static bool g_only_collect;
bool g_atrace_enable, g_debug_verbose;
static bool g_collect_pinfile = true, g_collect_specify_filetype;
bool g_pte_anon_swapin_enable = true, g_pte_fadvise_enable;
bool g_pte_mark_file_anon_enable = true;
bool g_swapin_expire_interruput_enable = true;
bool g_rlock_expire_interruput_enable = true;
unsigned int g_max_pte_segment_count = 0x190000, g_pte_max_active_apps = 50;
unsigned long long g_swapin_total_time_threshold = 30000, g_rlock_acquire_threshold = 2000;
module_param_named(lb_debug, g_lb_msg_level, int, 0644);
module_param_named(lb_only_collect, g_only_collect, bool, 0644);
module_param_named(lb_atrace_enable, g_atrace_enable, bool, 0644);
module_param_named(lb_collect_specify_filetype, g_collect_specify_filetype, bool, 0644);
module_param_named(lb_collect_pinfile, g_collect_pinfile, bool, 0644);
module_param_named(lb_debug_verbose, g_debug_verbose, bool, 0644);
module_param_named(lb_rlock_expire_interruput_enable, g_rlock_expire_interruput_enable, bool, 0644);
module_param_named(lb_swapin_expire_interruput_enable, g_swapin_expire_interruput_enable, bool, 0644);
module_param_named(lb_rlock_acquire_threshold, g_rlock_acquire_threshold, ullong, 0644);
module_param_named(lb_swapin_total_time_threshold, g_swapin_total_time_threshold, ullong, 0644);
module_param_named(lb_pte_mark_file_anon_enable, g_pte_mark_file_anon_enable, bool, 0644);
module_param_named(lb_pte_fadvise_enable, g_pte_fadvise_enable, bool, 0644);
module_param_named(lb_pte_anon_swapin_enable, g_pte_anon_swapin_enable, bool, 0644);
module_param_named(lb_pte_max_active_apps, g_pte_max_active_apps, uint, 0644);
module_param_named(lb_max_pte_segment_count, g_max_pte_segment_count, uint, 0644);
MODULE_PARM_DESC(lb_max_pte_segment_count, "max pte segment count to limit memory usage");
MODULE_PARM_DESC(lb_pte_max_active_apps, "max active apps");
MODULE_PARM_DESC(lb_pte_anon_swapin_enable, "anon swapin enable or not");
MODULE_PARM_DESC(lb_pte_fadvise_enable, "fadvise enable or not");
MODULE_PARM_DESC(lb_pte_mark_file_anon_enable, "mark pte_segment as anon and file or not");
MODULE_PARM_DESC(lb_swapin_total_time_threshold, "(in us) swapin time exceed this threshold will stop preread");
MODULE_PARM_DESC(lb_rlock_acquire_threshold, "(in us) mmap lock acquire time exceed this threshold will stop preread");
MODULE_PARM_DESC(lb_swapin_expire_interruput_enable, "interrupt preread when swapin time exceed threshold");
MODULE_PARM_DESC(lb_rlock_expire_interruput_enable, "interrupt preread when mmap lock acquire time exceed threshold");
MODULE_PARM_DESC(lb_debug_verbose, "iorap debug verbose");
MODULE_PARM_DESC(lb_collect_pinfile, "iorap collect pinfile");
MODULE_PARM_DESC(lb_collect_specify_filetype, "iorap only collect specify filetype enable");
MODULE_PARM_DESC(lb_atrace_enable, "iorap atrace enable");
MODULE_PARM_DESC(lb_only_collect, "iorap only collect");
MODULE_PARM_DESC(lb_debug, "iorap debug msg level");

struct lb_fileinfo {
	struct list_head list;
	int lives;
	u64 ino;
	u32 dev;
	char *path;
	struct timespec64 mtime;
	u64 unused;
	u64 pages;
	bool moved, used;
	u32 memory;
	u32 bitmap_words;
	unsigned long *bitmap;
};
struct lb_package {
	struct list_head list, files;
	struct lb_command app;
	struct mutex package_lock;
	u32 file_count, file_size, new_size, hit_rate, memory;
	bool cleared;
	atomic_t refs;
};
struct lb_inode_ref {
	u64 ino;
	u32 dev;
	struct file *file;
};
struct lb_page_record {
	struct file *file;
	pgoff_t index;
	u32 count;
};
struct lb_manager {
	spinlock_t record_lock;
	struct mutex package_list_lock;
	struct task_struct *compiler;
	char path[4096];
	struct list_head packages;
	struct lb_inode_ref inodes[2048];
	u16 inode_count;
	struct lb_page_record *records;
	u32 record_count;
	wait_queue_head_t compiler_wait;
	struct mutex compiler_lock;
	struct delayed_work work;
	u64 memory;
	u32 package_count;
	unsigned long collect_start;
};
static struct lb_manager *g_manager;
static atomic_t collecting_ = ATOMIC_INIT(0);
static struct {
	struct lb_package *package;
	union {
		struct {
			u32 count;
			pid_t pids[5];
		};
		u64 words[3];
	};
} g_active_package;
static struct lb_command debug_owner;
static struct {
	int major;
	u32 unused;
	struct class *class;
	struct device *device;
	void *unused_pointer;
} g_lb_data;

/* free_one_fileinfo .text 0x2938..0x29fc */
static void free_one_fileinfo(struct lb_package *p, struct lb_fileinfo *f)
{
	list_del(&f->list);
	p->file_count--;
	p->memory -= f->memory;
	g_manager->memory -= f->memory;
	kfree(f->path);
	kfree(f->bitmap);
	kfree(f);
}
/* package_info_do_destroy 0x2a04..0x2aec; subtract twice is stock. */
static bool package_info_do_destroy(struct lb_package *p)
{
	struct lb_fileinfo *f, *next;

	mutex_lock(&g_manager->package_list_lock);
	list_del(&p->list);
	g_manager->package_count--;
	g_manager->memory -= p->memory;
	mutex_unlock(&g_manager->package_list_lock);
	list_for_each_entry_safe(f, next, &p->files, list)
		free_one_fileinfo(p, f);
	kfree(p);
	return true;
}
static bool package_info_put(struct lb_package *p)
{
	if (atomic_dec_and_test(&p->refs)) {
		package_info_do_destroy(p);
		return true;
	}
	return false;
}
/* mi_lb_is_in_package_list 0x74..0xc0 checks only the first node. */
static bool mi_lb_is_in_package_list(struct lb_package *p)
{
	bool found;

	mutex_lock(&g_manager->package_list_lock);
	found = g_manager->packages.next == &p->list;
	mutex_unlock(&g_manager->package_list_lock);
	return found;
}
static struct lb_package *mi_lb_get_package_info(struct lb_command *cmd, bool move)
{
	struct lb_package *p, *found = NULL;

	mutex_lock(&g_manager->package_list_lock);
	list_for_each_entry(p, &g_manager->packages, list) {
		if (p->app.uid != cmd->uid || strcmp(p->app.name, cmd->name))
			continue;
		if (move)
			list_move(&p->list, &g_manager->packages);
		atomic_inc(&p->refs);
		found = p;
		break;
	}
	mutex_unlock(&g_manager->package_list_lock);
	return found;
}
static struct lb_package *mi_lb_get_oldest_package_info(void)
{
	struct lb_package *p;

	mutex_lock(&g_manager->package_list_lock);
	p = list_last_entry(&g_manager->packages, struct lb_package, list);
	mutex_unlock(&g_manager->package_list_lock);
	return p;
}
/* mi_lb_add_package_info 0xc4..0x22c compares only the head package. */
static int mi_lb_add_package_info(struct lb_package *p)
{
	struct lb_package *head;

	mutex_lock(&g_manager->package_list_lock);
	if (!list_empty(&g_manager->packages)) {
		head = list_first_entry(&g_manager->packages, struct lb_package, list);
		if (head->app.uid == p->app.uid && !strcmp(head->app.name, p->app.name))
			goto out;
	}
	list_add(&p->list, &g_manager->packages);
	atomic_inc(&p->refs);
	g_manager->package_count++;
 out:
	mutex_unlock(&g_manager->package_list_lock);
	return 0;
}
/* mi_lb_record_buf_clear 0x24f8..0x2618; no locking in stock. */
static void mi_lb_record_buf_clear(bool trace)
{
	unsigned int i;
	char *path;

	for (i = 0; i < g_manager->inode_count; i++) {
		BUG_ON(i >= 2048);
		if (!g_manager->inodes[i].file)
			continue;
		if (g_atrace_enable && trace) {
			path = file_path(g_manager->inodes[i].file, g_manager->path, 4096);
			LB_D("mi_lb_record_buf_clear", 0xc5, "collected file: %s", path);
		}
		fput(g_manager->inodes[i].file);
		g_manager->inodes[i].file = NULL;
	}
	g_manager->inode_count = 0;
	g_manager->record_count = 0;
}
static void clear_active(void)
{
	g_active_package.words[1] = 0;
	g_active_package.words[2] = 0;
	g_active_package.package = NULL;
	g_active_package.words[0] = 0;
}
/* mi_lb_dealwith_collect_cmd .text 0x1264..0x1848 */
static int mi_lb_dealwith_collect_cmd(int action, struct lb_command *cmd)
{
	struct lb_package *p = g_active_package.package;
	int old;
	size_t len;

	if (action == 4) {
		if (!atomic_read(&collecting_)) {
			LB_D("mi_lb_dealwith_collect_cmd", 0x183, "abort: the app[%s] is not collecting", cmd->name);
			return 0;
		}
		if (!p) {
			LB_E("mb_lb_dealwith_collect_abort", 0xeb, "active package is NULL");
			return 0;
		}
		clear_active();
		cancel_delayed_work_sync(&g_manager->work);
		if (p->app.uid != cmd->uid)
			return 0;
		len = strnlen(cmd->name, 256);
		if (len == 256)
			fortify_panic("__fortify_strlen");
		if (strncmp(p->app.name, cmd->name, len))
			return 0;
		mi_lb_record_buf_clear(false);
		if (!mi_lb_is_in_package_list(p)) {
			LB_D("mb_lb_dealwith_collect_abort", 0xf7, "active package %s not in package list", p->app.name);
			g_manager->memory -= p->memory;
			kfree(p);
		} else {
			package_info_put(p);
		}
		atomic_set(&collecting_, 0);
		return 0;
	}
	if (action == 3) {
		if (!atomic_read(&collecting_)) {
			LB_D("mi_lb_dealwith_collect_cmd", 0x17c, "finish: the app[%s] is not collecting", cmd->name);
			return 0;
		}
		if (!p) {
			LB_E("mb_lb_dealwith_collect_finish", 0xd7, "active package is NULL");
			return 0;
		}
		if (p->app.uid == cmd->uid && !strcmp(p->app.name, cmd->name)) {
			cancel_delayed_work_sync(&g_manager->work);
			wake_up_all(&g_manager->compiler_wait);
		} else {
			LB_E("mb_lb_dealwith_collect_finish", 0xe1, "finish collecting app[%s] != active app[%s]",
			     p->app.name, cmd->name);
		}
		return 0;
	}
	if (action != 1) {
		LB_E("mi_lb_dealwith_collect_cmd", 0x187, "unknown cold start cmd is %u", action);
		return 0;
	}
	old = atomic_read(&collecting_);
	for (;;) {
		if (old == 1) {
			p = g_active_package.package;
			if (p && !strcmp(p->app.name, cmd->name) && g_active_package.count < 5)
				g_active_package.pids[g_active_package.count++] = cmd->pid;
			return 0;
		}
		if (atomic_try_cmpxchg(&collecting_, &old, old + 1))
			break;
	}
	mi_lb_record_buf_clear(false);
	p = mi_lb_get_package_info(cmd, true);
	if (!p) {
		if (g_manager->package_count >= 100) {
			struct lb_package *oldest = mi_lb_get_oldest_package_info();

			if (oldest)
				package_info_put(oldest);
			else
				LB_E("mb_lb_dealwith_collect_start", 0x14a, "no old package to free");
		}
		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p) {
			LB_E("mb_lb_dealwith_collect_start", 0x151, "alloc package info fail");
			return 0; /* collecting_ remains set in stock. */
		}
		INIT_LIST_HEAD(&p->files);
		mutex_init(&p->package_lock);
		p->memory = 0x178;
		atomic_inc(&p->refs);
	}
	memcpy(&p->app, cmd, sizeof(*cmd));
	p->file_size = p->new_size = 0;
	g_active_package.package = p;
	g_manager->collect_start = jiffies;
	queue_delayed_work(system_wq, &g_manager->work, 500);
	if (g_active_package.package && g_active_package.count < 5)
		g_active_package.pids[g_active_package.count++] = cmd->pid;
	return 0;
}
/* mi_lb_pagecache_collect 0xf14..0x1228 */
static void mi_lb_pagecache_collect(struct file *file, pgoff_t index, u32 count)
{
	struct inode *inode = file_inode(file);
	struct file *held = NULL;
	u32 i;

	if (g_atrace_enable && debug_owner.uid && __kuid_val(current_uid()) == debug_owner.uid) {
		char *path = file_path(file, g_manager->path, 4096);

		if (!IS_ERR(path))
			LB_D("mi_lb_pagecache_collect", 0x4c, "collect file %s", path);
	}
	if (!g_active_package.package || !g_active_package.count)
		return;
	for (i = 0; i < g_active_package.count; i++) {
		BUG_ON(i >= 5);
		if (g_active_package.pids[i] == current->pid)
			break;
	}
	if (i == g_active_package.count || (long)(g_manager->collect_start - jiffies + 500) < 0)
		return;
	if (!inode->i_sb || (inode->i_sb->s_magic != 0xef53 &&
		inode->i_sb->s_magic != 0xf2f52010 && inode->i_sb->s_magic != 0xe0f5e1e2))
		return;
	spin_lock(&g_manager->record_lock);
	for (i = 0; i < g_manager->inode_count; i++) {
		BUG_ON(i >= 2048);
		if (g_manager->inodes[i].dev == inode->i_sb->s_dev &&
		    g_manager->inodes[i].ino == inode->i_ino) {
			held = g_manager->inodes[i].file;
			break;
		}
	}
	if (i < 2048 && i == g_manager->inode_count) {
		held = get_file(file);
		g_manager->inodes[i].file = held;
		g_manager->inodes[i].dev = inode->i_sb->s_dev;
		g_manager->inodes[i].ino = inode->i_ino;
		g_manager->inode_count++;
	}
	if (i != 2048 && g_manager->record_count < 131072) {
		struct lb_page_record *r = &g_manager->records[g_manager->record_count++];

		r->file = held;
		r->index = index;
		r->count = count;
	}
	spin_unlock(&g_manager->record_lock);
}
static void mi_lb_page_collect_on_readfile(void *unused, struct file *file, loff_t pos, size_t size)
{
	if (size)
		mi_lb_pagecache_collect(file, pos >> 12, (u32)(size + 4095) >> 12);
}
static void mi_lb_page_collect_on_pagefault(void *unused, struct file *file,
				pgoff_t first, pgoff_t last, vm_fault_t ret)
{
	if (first != last)
		mi_lb_pagecache_collect(file, first, (u32)last - (u32)first);
}

/* Inlined mi_lb_get_fileinfo/launch_reset_fileinfo in compiler_thread
 * 0x1c94..0x219c. Both cap the file at 2 GiB, independently of PAGE_SIZE. */
static bool lb_reset_fileinfo(struct lb_fileinfo *f, struct file *file)
{
	struct inode *inode = file_inode(file);
	u64 size = min_t(u64, i_size_read(inode), 0x80000000ULL);

	f->unused = 0;
	f->dev = inode->i_sb->s_dev;
	f->ino = inode->i_ino;
	f->mtime = inode_get_mtime(inode);
	f->lives = 3;
	f->pages = (size + 4095) >> 12;
	f->bitmap_words = (f->pages + 63) >> 6;
	f->bitmap = bitmap_zalloc(f->pages, GFP_KERNEL);
	return f->bitmap != NULL;
}
static bool lb_filetype_matches(struct file *file)
{
	static const char * const types[] = { "so", "apk", "dex", "vdex", "odex", "art", "jar", NULL };
	const char *name;
	size_t size, len, i;
	unsigned int type;

	if (!file->f_path.dentry || !file->f_path.dentry->d_name.name)
		return false;
	name = file->f_path.dentry->d_name.name;
	size = strlen(name);
	for (type = 0; types[type]; type++) {
		len = strlen(types[type]);
		if (len + 2 > size)
			continue;
		for (i = 1; i < size - len; i++)
			if (name[i] == '.' && !strncasecmp(name + i + 1, types[type], len))
				return true;
	}
	return false;
}
static struct lb_fileinfo *lb_get_fileinfo(struct lb_package *p, struct file *file)
{
	struct inode *inode = file_inode(file);
	struct timespec64 mtime = inode_get_mtime(inode);
	struct lb_fileinfo *f;
	char *path;
	size_t len;

	path = file_path(file, g_manager->path, 4096);
	if (IS_ERR(path)) {
		LB_E("mi_lb_get_fileinfo", 0x1d4, "find filepath error\n");
		return NULL;
	}
	list_for_each_entry(f, &p->files, list) {
		if (f->dev != inode->i_sb->s_dev || strcmp(f->path, path))
			continue;
		if (!f->moved) {
			list_move(&f->list, &p->files);
			f->moved = true;
		}
		f->used = true;
		/* Stock ignores sub-second mtime changes. */
		if (f->ino == inode->i_ino && f->mtime.tv_sec == mtime.tv_sec)
			return f;
		kfree(f->bitmap);
		if (lb_reset_fileinfo(f, file))
			return f;
		LB_E("launch_reset_fileinfo", 0x1bc, "alloc file_info bitmaps err");
		/* Stock OOM removal skips file_count/memory accounting. */
		kfree(f->path);
		list_del(&f->list);
		kfree(f);
		return NULL;
	}
	if (!g_collect_pinfile && (!strncmp(path, "/system", 7) || !strncmp(path, "/system_ext", 11)))
		return NULL;
	if (g_collect_specify_filetype && !lb_filetype_matches(file))
		return NULL;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		LB_E("mi_lb_get_fileinfo", 0x1eb, "alloc file_info err");
		return NULL;
	}
	f->moved = f->used = true;
	if (!lb_reset_fileinfo(f, file)) {
		LB_E("mi_lb_get_fileinfo", 0x1fb, "alloc file_info bitmaps err");
		kfree(f);
		return NULL;
	}
	len = (int)strlen(path) + 1;
	f->path = kzalloc(len, GFP_KERNEL);
	if (!f->path) {
		LB_E("mi_lb_get_fileinfo", 0x202, "info->path alloc fail\n");
		bitmap_free(f->bitmap);
		kfree(f);
		return NULL;
	}
	strncpy(f->path, path, len);
	list_add(&f->list, &p->files);
	p->file_count++;
	f->memory = len + f->bitmap_words * 8 + 0x68;
	p->memory += f->memory;
	g_manager->memory += f->memory;
	return f;
}
/* compiler_thread .text 0x1a7c..0x2408: unconditionally sleeps before work. */
static int mi_lb_compiler_thread(void *data)
{
	struct lb_manager *manager = data;
	struct lb_package *p;
	struct lb_fileinfo *f, *next;
	unsigned int i, j;

	if (!manager) {
		LB_E("mi_lb_compiler_thread", 0x25d, "data is null");
		return -EINVAL;
	}
	LB_D("mi_lb_compiler_thread", 0x261, "collecting_: %d", atomic_read(&collecting_));
	while (!kthread_should_stop()) {
		DEFINE_WAIT(wait);

		prepare_to_wait(&manager->compiler_wait, &wait, TASK_UNINTERRUPTIBLE);
		schedule();
		finish_wait(&manager->compiler_wait, &wait);
		if (!atomic_read(&collecting_))
			continue;
		p = g_active_package.package;
		if (!p) {
			LB_E("mi_lb_compiler_do_work", 0x23e, "package is null");
			continue;
		}
		mutex_lock(&g_manager->compiler_lock);
		clear_active();
		list_for_each_entry(f, &p->files, list)
			f->used = false;
		for (i = 0; i < g_manager->record_count; i++) {
			struct lb_page_record *r = &g_manager->records[i];

			f = lb_get_fileinfo(p, r->file);
			if (!f || r->index + r->count > f->pages)
				continue;
			if (g_debug_verbose) {
				u32 bit, end = (u32)r->index + r->count;

				for (bit = (u32)r->index; bit < end; bit++)
					if (!test_bit(bit, f->bitmap))
						p->new_size += 4096;
			}
			bitmap_set(f->bitmap, (int)r->index, r->count);
		}
		mi_lb_add_package_info(p);
		list_for_each_entry_safe(f, next, &p->files, list) {
			if (g_debug_verbose)
				p->file_size += bitmap_weight(f->bitmap, f->pages) * 4096;
			f->moved = false;
			for (j = 0; j < g_manager->inode_count; j++) {
				BUG_ON(j >= 2048);
				if (g_manager->inodes[j].dev == f->dev && g_manager->inodes[j].ino == f->ino)
					break;
			}
			if (j < g_manager->inode_count)
				f->lives = 3;
			else if (--f->lives == 0)
				free_one_fileinfo(p, f);
		}
		mi_lb_record_buf_clear(true);
		package_info_put(p);
		atomic_set(&collecting_, 0);
		mutex_unlock(&g_manager->compiler_lock);
		if (g_debug_verbose) {
			/* Retain the stock access after package_info_put(). */
			LB_D("mi_lb_compiler_do_work", 0x24f, "compiler finish: total_memory %lu apps %u",
			     (unsigned long)g_manager->memory, g_manager->package_count);
			p->hit_rate = 100 - (p->file_size ? (u64)p->new_size * 100 / p->file_size : 0);
			LB_D("mi_lb_compiler_do_work", 0x254,
			     "current app %s, This app load io: %u bytes, total io: %u bytes, hit_rate: %d%%",
			     p->app.name, p->new_size, p->file_size, p->hit_rate);
		}
	}
	return 0;
}
static void mi_lb_do_compile(struct work_struct *work)
{
	wake_up_all(&g_manager->compiler_wait);
}
/* mi_lb_init_manager 0x184c..0x1a3c */
static int mi_lb_init_manager(void)
{
	struct task_struct *thread;
	int ret;

	g_manager = kzalloc(sizeof(*g_manager), GFP_KERNEL);
	if (!g_manager)
		return -ENOMEM;
	g_manager->records = kvmalloc(0x300000, GFP_KERNEL);
	if (!g_manager->records) {
		kfree(g_manager);
		return -ENOMEM;
	}
	spin_lock_init(&g_manager->record_lock);
	g_manager->memory = 0x30d150;
	mutex_init(&g_manager->compiler_lock);
	mutex_init(&g_manager->package_list_lock);
	INIT_LIST_HEAD(&g_manager->packages);
	INIT_DELAYED_WORK(&g_manager->work, mi_lb_do_compile);
	init_waitqueue_head(&g_manager->compiler_wait);
	thread = kthread_create(mi_lb_compiler_thread, g_manager, "lb:g_manager");
	g_manager->compiler = thread;
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		LB_E("mi_lb_init_manager", 0x28e, "create compiler_thread fail, ret: %d", ret);
		kvfree(g_manager->records);
		kfree(g_manager);
		return ret;
	}
	wake_up_process(thread);
	clear_active();
	return 0;
}
/* mi_lb_exit_manager 0x240c..0x24f4: records cleared before thread stop;
 * delayed work and cached package objects remain allocated. */
static int mi_lb_exit_manager(void)
{
	mi_lb_record_buf_clear(false);
	if (g_manager) {
		kthread_stop(g_manager->compiler);
		wake_up(&g_manager->compiler_wait);
		kvfree(g_manager->records);
		kfree(g_manager);
	}
	return 0;
}
static int mi_lb_dealwith_clear_cmd(struct lb_command *cmd)
{
	struct lb_package *p;

	LB_D("mi_lb_dealwith_clear_cmd", 0x39,
	     "clear cmd : package uid: %d, pid: %d, packagename: %s", cmd->uid, cmd->pid, cmd->name);
	p = mi_lb_get_package_info(cmd, false);
	if (!p) {
		LB_D("mi_lb_dealwith_clear_cmd", 0x3d, "app not found, don't clear");
		return -1;
	}
	mutex_lock(&p->package_lock);
	if (!p->cleared) {
		p->cleared = true;
		mutex_unlock(&p->package_lock);
		package_info_put(p);
	} else {
		mutex_unlock(&p->package_lock);
		LB_D("mi_lb_dealwith_clear_cmd", 0x43, "app is cleared, don't clear");
	}
	package_info_put(p);
	return 0;
}
/* preread_size .text 0x261c..0x2734 */
static int mi_lb_dealwith_preread_size_cmd(struct lb_size_command *cmd)
{
	struct lb_package *p;
	struct lb_fileinfo *f;
	u64 size = 0x118;
	int ret;

	mutex_lock(&g_manager->compiler_lock);
	p = mi_lb_get_package_info(&cmd->app, false);
	if (!p) {
		LB_D("mi_lb_dealwith_preread_size_cmd", 0x5f, "app first cold start, No pre-reading");
		ret = -1;
	} else if (list_empty(&p->files)) {
		LB_D("mi_lb_dealwith_preread_size_cmd", 0x66, "app has collected, but no pre-reading files");
		ret = -2; /* Stock leaks the lookup reference here. */
	} else {
		list_for_each_entry(f, &p->files, list)
			if (f->used)
				size += 0x128 + (u64)f->bitmap_words * 8;
		cmd->size = size + 0x110;
		package_info_put(p);
		ret = 0;
	}
	mutex_unlock(&g_manager->compiler_lock);
	return ret;
}
/* preread_info .text 0x2738..0x2934; size stays the supplied capacity. */
static int mi_lb_dealwith_preread_info_cmd(struct lb_info_command *cmd)
{
	struct lb_package *p;
	struct lb_fileinfo *f;
	u64 offset = 0x118, end;
	size_t len;

	mutex_lock(&g_manager->compiler_lock);
	p = mi_lb_get_package_info(&cmd->app, false);
	if (!p) {
		mutex_unlock(&g_manager->compiler_lock);
		LB_D("mi_lb_dealwith_preread_info_cmd", 0x7c, "app first cold start, No pre-reading");
		return -1;
	}
	cmd->file_count = 0;
	list_for_each_entry_reverse(f, &p->files, list) {
		struct lb_wire_file *out;

		if (!f->used)
			continue;
		end = offset + 0x128 + (u64)f->bitmap_words * 8;
		if (cmd->size < end) {
			LB_E("get_app_pre_filesinfo", 0x26, "Exceeded preread_info memory size %zu", (size_t)cmd->size);
			break;
		}
		out = (void *)cmd + offset;
		out->mtime_sec = f->mtime.tv_sec;
		out->mtime_nsec = f->mtime.tv_nsec;
		out->ino = f->ino;
		out->pages = f->pages;
		len = strlen(f->path);
		if (len <= 255) {
			strncpy(out->path, f->path, len);
			out->path[len] = '\0';
		} else {
			LB_E("get_app_pre_filesinfo", 0x35, "file path is too long %s", f->path);
			out->path[0] = '\0';
		}
		out->bitmap_words = f->bitmap_words;
		memcpy(out->bitmap, f->bitmap, (u64)f->bitmap_words * 8);
		cmd->file_count++;
		offset = end;
	}
	package_info_put(p);
	mutex_unlock(&g_manager->compiler_lock);
	return 0;
}
static int mi_lb_dealwith_get_app_names(struct lb_app_names *out)
{
	struct lb_package *p;
	u32 i = 0;

	mutex_lock(&g_manager->package_list_lock);
	list_for_each_entry(p, &g_manager->packages, list) {
		memcpy(&out->apps[i++], &p->app, 0x108);
		if (i == 100)
			break;
	}
	out->count = i;
	mutex_unlock(&g_manager->package_list_lock);
	return 0;
}
/* mi_lb_ioctl .text 0x444..0xcc8. compat uses the same 64-bit wire layout. */
static long mi_lb_ioctl(struct file *file, unsigned int request, unsigned long arg)
{
	struct lb_command cmd = {};
	struct lb_size_command size_cmd = {};
	struct lb_info_command *info;
	struct lb_app_names *names;
	void __user *user = (void __user *)arg;
	u64 size;
	int ret;

	switch (request) {
	case 0xc1084c01:
	case 0xc1084c02:
	case 0xc1084c03:
	case 0xc1084c05:
	case 0xc1084c06:
	case 0xc1084c10:
	case 0xc1084c11:
	case 0xc1084c13:
		if (copy_from_user(&cmd, user, 0x108)) {
			unsigned int line = 0;

			switch (request) {
			case 0xc1084c01: line = 0x61; break;
			case 0xc1084c02: line = 0x55; break;
			case 0xc1084c03: line = 0xb5; break;
			case 0xc1084c05: line = 0xa1; break;
			case 0xc1084c06: line = 0xab; break;
			case 0xc1084c10: line = 0xcc; break;
			case 0xc1084c11: line = 0xd5; break;
			case 0xc1084c13: line = 0xde; break;
			}
			if (request == 0xc1084c02)
				LB_E("mi_lb_ioctl", line, "LB_PREREAD copy from user fail");
			else
				LB_E("mi_lb_ioctl", line, "copy from user fail");
			return request == 0xc1084c06 || request == 0xc1084c13 ? -1 : -EFAULT;
		}
		break;
	case 0xc1084c07:
		if (g_only_collect)
			return -1;
		if (copy_from_user(&size_cmd, user, 0x110)) {
			LB_E("mi_lb_ioctl", 0x6d, "copy from user fail");
			return -EFAULT;
		}
		ret = mi_lb_dealwith_preread_size_cmd(&size_cmd);
		if (ret < -1) {
			LB_E("mi_lb_ioctl", 0x72, "lb preread size fail");
			return -1;
		}
		if (ret == -1) {
			LB_E("mi_lb_ioctl", 0x76, "lb app %s cold start", size_cmd.app.name);
			return -2;
		}
		if (copy_to_user(user, &size_cmd, 0x110)) {
			LB_E("mi_lb_ioctl", 0x7a, "LB_GET_PREREAD_INFO_SIZE: copy to user fail");
			return -EFAULT;
		}
		return ret;
	case 0xc1084c08:
		if (get_user(size, (u64 __user *)(user + 0x110))) {
			LB_E("mi_lb_ioctl", 0x81, "get_user fail");
			return -EFAULT;
		}
		if (size > 0xa00000) {
			LB_E("mi_lb_ioctl", 0x85, "buf_size %lu too large", (unsigned long)size);
			return -EINVAL;
		}
		info = kvmalloc(size + 0x118, GFP_KERNEL);
		if (!info) {
			LB_E("mi_lb_ioctl", 0x8b, "alloc app_preread_info err");
			return -ENOMEM;
		}
		if (copy_from_user(info, user, 0x118)) {
			LB_E("mi_lb_ioctl", 0x8f, "copy from user fail");
			kvfree(info);
			return -EFAULT;
		}
		ret = mi_lb_dealwith_preread_info_cmd(info);
		if (ret) {
			LB_E("mi_lb_ioctl", 0x95, "lb pre read fail");
			kvfree(info);
			return -1;
		}
		ret = copy_to_user(user, info, info->size) ? -EFAULT : 0;
		if (ret)
			LB_E("mi_lb_ioctl", 0x9a, "LB_GET_PREREAD_INFO_SIZE: copy to user fail");
		kvfree(info);
		return ret;
	case 0xc1084c09:
		names = kvmalloc(0x6724, GFP_KERNEL);
		if (!names) {
			LB_E("mi_lb_ioctl", 0xbe, "alloc app_list err");
			return -ENOMEM;
		}
		ret = mi_lb_dealwith_get_app_names(names);
		if (ret)
			LB_E("mi_lb_ioctl", 0xc3, "lb get app names fail");
		if (copy_to_user(user, names, 0x6724)) {
			LB_E("mi_lb_ioctl", 0xc5, "LB_GET_APP_NAMES: copy to user fail");
			ret = -EFAULT;
		}
		kvfree(names);
		return ret;
	default:
		LB_E("mi_lb_ioctl", 0xe6, "unknown cmd, cmd is %u", request);
		return -ENOIOCTLCMD;
	}
	switch (request) {
	case 0xc1084c01:
		ret = mi_lb_dealwith_collect_cmd(1, &cmd);
		if (ret)
			LB_E("mi_lb_ioctl", 0x66, "lb collect cmd fail");
		return ret;
	case 0xc1084c02:
		debug_owner.uid = cmd.uid;
		return 0;
	case 0xc1084c03:
		mi_lb_dealwith_pte_clean_cmd(&cmd);
		mi_lb_dealwith_clear_cmd(&cmd);
		return 0;
	case 0xc1084c05:
		debug_owner.uid = -1;
		ret = mi_lb_dealwith_collect_cmd(3, &cmd);
		if (ret)
			LB_E("mi_lb_ioctl", 0xa7, "lb collect finish fail");
		return ret;
	case 0xc1084c06:
		debug_owner.uid = -1;
		ret = mi_lb_dealwith_collect_cmd(4, &cmd);
		if (ret)
			LB_E("mi_lb_ioctl", 0xb1, "lb collect abort fail");
		return ret;
	case 0xc1084c10:
		ret = mi_lb_dealwith_pte_snapshot_cmd(&cmd);
		if (ret)
			LB_E("mi_lb_ioctl", 0xd1, "lb pte snapshot fail");
		return ret;
	case 0xc1084c11:
		ret = mi_lb_dealwith_pte_preread_cmd(&cmd);
		if (ret)
			LB_E("mi_lb_ioctl", 0xda, "lb pte preread fail");
		return ret;
	case 0xc1084c13:
		ret = mi_lb_dealwith_pte_interrupt_cmd(&cmd);
		if (ret)
			LB_E("mi_lb_ioctl", 0xe3, "lb pte interrupt fail");
		return ret;
	}
	return -ENOIOCTLCMD;
}
static const struct file_operations lb_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mi_lb_ioctl,
	.compat_ioctl = mi_lb_ioctl,
};
static int lb_collect_show(struct seq_file *s, void *unused)
{
	struct lb_package *p;
	int i = 0;

	mutex_lock(&g_manager->package_list_lock);
	if (list_empty(&g_manager->packages)) {
		seq_puts(s, "g_cold_record_list_user0 is empty\n");
	} else {
		seq_printf(s, "total memory size %lu app_num: %u\n", (unsigned long)g_manager->memory,
			   g_manager->package_count);
		list_for_each_entry(p, &g_manager->packages, list)
			seq_printf(s, "[%d] app:%s, uid:%d, file num: %d, file size: %u, hit rate: %d%%, mm size: %d\n",
				i++, p->app.name, p->app.uid, p->file_count, p->file_size,
				p->hit_rate, p->memory);
	}
	mutex_unlock(&g_manager->package_list_lock);
	return 0;
}
static int lb_collect_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, lb_collect_show, NULL);
}
static int lb_hot_collect_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, lb_hot_collect_show, NULL);
}
static const struct proc_ops lb_collect_file_fops = {
	.proc_open = lb_collect_file_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
static const struct proc_ops lb_hot_collect_file_fops = {
	.proc_open = lb_hot_collect_file_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
static int mi_lb_dbg_init(void)
{
	struct proc_dir_entry *dir = proc_mkdir("iorap_node", NULL);

	if (!dir) {
		LB_E("mi_lb_create_proc", 0x93, "proc_mkdir failed\n");
		LB_E("mi_lb_dbg_init", 0xb0, "lb_create_proc failed\n");
		return -ENOMEM;
	}
	if (!proc_create("collect", 0, dir, &lb_collect_file_fops)) {
		remove_proc_entry("iorap_node", NULL);
		LB_E("mi_lb_create_proc", 0x9a, "proc_create lb_collect failed\n");
		LB_E("mi_lb_dbg_init", 0xb0, "lb_create_proc failed\n");
		return -ENOMEM;
	}
	if (!proc_create("hot_collect", 0, dir, &lb_hot_collect_file_fops)) {
		remove_proc_entry("collect", NULL); /* stock passes the wrong parent. */
		remove_proc_entry("iorap_node", NULL);
		LB_E("mi_lb_create_proc", 0xa2, "proc_create lb_hot_collect failed\n");
		LB_E("mi_lb_dbg_init", 0xb0, "lb_create_proc failed\n");
		return -ENOMEM;
	}
	return 0;
}
/* lb_cold_exit .text 0x3bc..0x440; stock omits device_destroy/proc cleanup. */
static void lb_cold_exit(void)
{
	unregister_trace_android_vh_filemap_read(mi_lb_page_collect_on_readfile, NULL);
	unregister_trace_android_vh_filemap_map_pages_range(mi_lb_page_collect_on_pagefault, NULL);
	mi_lb_exit_manager();
	if (g_lb_data.class)
		class_destroy(g_lb_data.class);
	if (g_lb_data.device)
		unregister_chrdev(g_lb_data.major, "iorap_dev");
}
static int __init mi_lb_init(void)
{
	int ret;

	memset(&g_lb_data, 0, sizeof(g_lb_data));
	g_lb_data.major = register_chrdev(0, "iorap_dev", &lb_fops);
	if (g_lb_data.major < 0) {
		LB_E("mi_lb_chrdev_register", 0xfd, "register chrdev fail\n");
		LB_E("mi_lb_init", 0x139, "lb_cold_init failed\n");
		return -ENXIO;
	}
	g_lb_data.class = class_create("iorap");
	if (IS_ERR_OR_NULL(g_lb_data.class)) {
		LB_E("mi_lb_chrdev_register", 0x102, "class create fail");
		ret = (long)g_lb_data.class;
		unregister_chrdev(g_lb_data.major, "iorap_dev");
		if (ret) {
			LB_E("mi_lb_init", 0x139, "lb_cold_init failed\n");
			return ret;
		}
	} else {
		g_lb_data.device = device_create(g_lb_data.class, NULL, MKDEV(g_lb_data.major, 0), NULL, "iorap_dev");
		/* Stock tests class again rather than device_create's return. */
		if (IS_ERR_OR_NULL(g_lb_data.class)) {
			LB_E("mi_lb_chrdev_register", 0x10a, "device create fail");
			ret = (long)g_lb_data.class;
			unregister_chrdev(g_lb_data.major, "iorap_dev");
			if (ret) {
				LB_E("mi_lb_init", 0x139, "lb_cold_init failed\n");
				return ret;
			}
		} else {
			dev_set_drvdata(g_lb_data.device, &g_lb_data);
		}
	}
	ret = mi_lb_init_manager();
	if (ret) {
		LB_E("mi_lb_init", 0x139, "lb_cold_init failed\n");
		return ret;
	}
	ret = register_trace_android_vh_filemap_read(mi_lb_page_collect_on_readfile, NULL);
	if (ret) {
		LB_E("mi_lb_register_hooks", 0x33, "register hook mi_lb_page_collect_on_readfile failed");
		LB_E("mi_lb_init", 0x139, "lb_cold_init failed\n");
		return ret;
	}
	ret = register_trace_android_vh_filemap_map_pages_range(mi_lb_page_collect_on_pagefault, NULL);
	if (ret) {
		LB_E("mi_lb_register_hooks", 0x3a, "register hook mi_lb_page_collect_on_pagefault failed");
		unregister_trace_android_vh_filemap_read(mi_lb_page_collect_on_readfile, NULL);
		LB_E("mi_lb_init", 0x139, "lb_cold_init failed\n");
		return ret;
	}
	ret = lb_hot_init();
	if (ret) {
		LB_E("mi_lb_init", 0x13e, "lb_cold_init failed\n");
		lb_cold_exit();
		return ret;
	}
	ret = mi_lb_dbg_init();
	if (ret) {
		LB_E("mi_lb_init", 0x144, "dbg init failed\n");
		lb_hot_exit();
		lb_cold_exit();
	}
	return ret;
}
static void __exit mi_lb_exit(void)
{
	lb_hot_exit();
	lb_cold_exit();
}
module_init(mi_lb_init);
module_exit(mi_lb_exit);
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
