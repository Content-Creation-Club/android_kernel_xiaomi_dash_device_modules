// SPDX-License-Identifier: GPL-2.0
/* Stock procfs and misc text ABI, recovered from io_monitor.ko. */
#include <linux/bug.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched/signal.h>
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include "io_monitor.h"

/* Stock uses sprintf for record payloads, including the 4 MiB misc buffer. */
#define OUTPUT(m, buf, off, fmt, ...) do { \
	if (m) seq_printf(m, fmt, ##__VA_ARGS__); \
	else (off) += sprintf((buf) + (off), fmt, ##__VA_ARGS__); \
} while (0)

static const char * const io_range_str[] = {
	"(0, 4K]", "(4K, 32K]", "(32K, 128K]", "(128K, 512K)", "[512K, +∞)",
};
static const char * const block_section_str[] = { "Q2G", "G2I", "I2D", "D2C", "Q2C" };

static __always_inline void io_proc_bounds_trap(void)
{
	/* Stock .text+0x26a4, 0x47b0 and 0x5678. */
	asm volatile("brk #0x5512");
	__builtin_unreachable();
}

static __always_inline void io_output_bounds_trap(void)
{
	/* Stock .text+0x6eb4, reached before writing the '(' and NUL. */
	asm volatile("brk #1");
	__builtin_unreachable();
}

unsigned int f2fs_read_info_output(struct seq_file *m, char *buf, unsigned int off)
{
	unsigned int pid, cycle, task, kind;
	struct io_filemap *info;
	spinlock_t *lock;
	struct io_size_stat *s;
	if (m)
		seq_printf(m, "F2FS_READ:\n");
	else
		off += sprintf(buf + off, "\nF2FS_READ:\n");
	for (pid = 0; pid < IO_PID_MAX; pid++) {
		info = &f2fs_read_info.info[pid];
		if (!info->valid)
			continue;
		OUTPUT(m, buf, off, "timestamp utc_seconds uid ppid pcomm\n");
		OUTPUT(m, buf, off, "%llu %lld %d %d %s\n", info->common.timestamp,
			info->common.utc_seconds, info->common.uid, info->common.ppid, info->common.pcomm);
		for (cycle = 0; cycle < 2; cycle++) {
			OUTPUT(m, buf, off, "\n%s:\n", sample_cycle_config[cycle].name);
			OUTPUT(m, buf, off, "task_type read_type total_cnt total_size avg_size max_size\n");
			for (task = 0; task < 2; task++)
				for (kind = 0; kind < 2; kind++) {
					lock = kind ? &f2fs_read_info.map_lock : &f2fs_read_info.read_lock;
					spin_lock(lock);
					s = &info->stats[task][kind][cycle];
					OUTPUT(m, buf, off, "%s %s %llu %llu %llu %llu\n",
						task ? "RenderThread" : "main_thread",
						kind ? "filemap_map_pages" : "filemap_read",
						s->count, s->total, s->avg, s->max);
					spin_unlock(lock);
				}
		}
	}
	return off;
}

int iostat_init(void)
{
	memset(&iostat_info, 0, sizeof(iostat_info));
	iostat_info.info = kvzalloc(0x280000, GFP_KERNEL);
	if (!iostat_info.info) {
		IO_LOG("iostat_mm_init", 0x115, "iostat_info pinfo kvzalloc failed!\n");
		IO_LOG("iostat_init", 0x137, "iostat_mm_init failed.\n");
		return -1;
	}
	iostat_info.top = kvzalloc(800, GFP_KERNEL);
	if (!iostat_info.top) {
		IO_LOG("iostat_mm_init", 0x11d, "iostat_info iotop kvzalloc failed!\n");
		kvfree(iostat_info.info);
		IO_LOG("iostat_init", 0x137, "iostat_mm_init failed.\n");
		return -1;
	}
	mutex_init(&iostat_info.lock);
	IO_LOG("iostat_init", 0x13c, "%s ok\n", "iostat_init");
	return 0;
}

void iostat_exit(void)
{
	iostat_enabled = false;
	if (iostat_info.info)
		kvfree(iostat_info.info);
	if (iostat_info.top)
		kvfree(iostat_info.top);
	IO_LOG("iostat_exit", 0x147, "%s\n", "iostat_exit");
}

static int compare(const void *a, const void *b)
{
	const struct io_pio *left = a, *right = b;
	/* Stock compares only signed low 32 bits of cumulative write_bytes. */
	return (int)((u32)right->write_bytes - (u32)left->write_bytes);
}

static void ioacct_add(struct task_io_accounting *a, const struct task_io_accounting *b)
{
	a->rchar += b->rchar;
	a->wchar += b->wchar;
	a->syscr += b->syscr;
	a->syscw += b->syscw;
	a->read_bytes += b->read_bytes;
	a->write_bytes += b->write_bytes;
	a->cancelled_write_bytes += b->cancelled_write_bytes;
}

/* .text+0x731c..0x7910: retains stale PID slots and unlocked process traversal. */
unsigned int iostat_output(struct seq_file *m, char *buf, unsigned int off)
{
	struct task_struct *task, *thread;
	struct task_io_accounting acct;
	struct io_pio *p, *sorted;
	unsigned long flags;
	int seq;
	unsigned int i;
	mutex_lock(&iostat_info.lock);
	for_each_process(task) {
		if (!task->pid || task->pid >= IO_PID_MAX)
			continue;
		p = &iostat_info.info[task->pid];
		p->pid = task->pid;
		get_task_comm(p->comm, task);
		if (down_read_killable(&task->signal->exec_update_lock))
			continue;
		if (task->exit_signal < 0) {
			acct = task->ioac;
		} else {
			seq = 1;
			rcu_read_lock();
			do {
				seq++;
				flags = read_seqbegin_or_lock_irqsave(&task->signal->stats_lock, &seq);
				acct = task->signal->ioac;
				for_each_thread(task, thread)
					ioacct_add(&acct, &thread->ioac);
			} while (need_seqretry(&task->signal->stats_lock, seq));
			done_seqretry_irqrestore(&task->signal->stats_lock, seq, flags);
			rcu_read_unlock();
		}
		p->rchar = acct.rchar;
		p->wchar = acct.wchar;
		p->syscr = acct.syscr;
		p->syscw = acct.syscw;
		p->read_bytes = acct.read_bytes;
		p->write_bytes = acct.write_bytes;
		p->cwrite_bytes = acct.cancelled_write_bytes;
		up_read(&task->signal->exec_update_lock);
		p->valid = true;
	}
	sorted = kvzalloc(0x280000, GFP_KERNEL);
	if (!sorted) {
		IO_LOG("sort_pio_info", 0x8d, "tpinfo kvzalloc failed!\n");
		goto out;
	}
	memcpy(sorted, iostat_info.info, 0x280000);
	sort(sorted, IO_PID_MAX, sizeof(*sorted), compare, NULL);
	memcpy(iostat_info.top, sorted, 800);
	kvfree(sorted);
	OUTPUT(m, buf, off, "\niotop10:\npid comm rchar wchar syscr syscw read_bytes write_bytes cwrite_bytes\n");
	for (i = 0; i < 10; i++) {
		p = &iostat_info.top[i];
		if (p->valid)
			OUTPUT(m, buf, off, "%d %s %llu %llu %llu %llu %llu %llu %llu\n",
				p->pid, p->comm, p->rchar, p->wchar, p->syscr, p->syscw,
				p->read_bytes, p->write_bytes, p->cwrite_bytes);
	}
out:
	mutex_unlock(&iostat_info.lock);
	return off;
}

static void elapsed_show(struct seq_file *m, struct io_fsync *info)
{
	unsigned int i;
	seq_printf(m, "cycle\ttotal_cnt\ttotal_time\tavg_time\tmax_time\tlast_time\n");
	spin_lock(&info->lock);
	for (i = 0; i < 2; i++) {
		struct io_elapsed *s = &info->cycle[i];
		seq_printf(m, "%s\t%llu\t%llu\t%llu\t%llu\t%llu\n", sample_cycle_config[i].name,
			s->count, s->total, s->avg, s->max, s->last);
	}
	spin_unlock(&info->lock);
}

static void gc_show(struct seq_file *m)
{
	unsigned int i;
	struct io_gc_elapsed *fg, *bg;
	seq_printf(m, "cycle\ttotal_cnt(fg/bg)\ttotal_seg_cnt(fg/bg)\ttotal_time(fg/bg)\tavg_time(fg/bg)\tmax_time(fg/bg)\tlast_time(fg/bg)\n");
	spin_lock(&f2fs_gc_info.lock);
	for (i = 0; i < 2; i++) {
		fg = &f2fs_gc_info.cycle[i][1];
		bg = &f2fs_gc_info.cycle[i][0];
		seq_printf(m, "%s\t%llu/%llu\t%llu/%llu\t%llu/%llu\t%llu/%llu\t%llu/%llu\t%llu/%llu\n",
			sample_cycle_config[i].name, fg->count, bg->count, fg->segments, bg->segments,
			fg->total, bg->total, fg->avg, bg->avg, fg->max, bg->max, fg->last, bg->last);
	}
	spin_unlock(&f2fs_gc_info.lock);
}

static void block_show(struct seq_file *m)
{
	u64 count = 0, size = 0, time = 0, maximum = 0;
	unsigned int rw, cycle, range, section, lat;
	struct io_block_bucket *s;
	unsigned long flags;
	spin_lock_irqsave(&block_info.lock, flags);
	for (rw = 0; rw < 2; rw++) {
		seq_printf(m, "\n%s:\n", rw ? "Write" : "Read");
		seq_printf(m, "all_size_dist(second_10,minutes_1,,)\tall_total_count\tall_avg_size\tall_avg_time\tall_max_time\n");
		for (cycle = 0; cycle < 2; cycle++) {
			seq_printf(m, "(");
			for (range = 0; range < 5; range++) {
				s = &block_info.bucket[rw][cycle][range];
				seq_printf(m, "%llu,", s->total_size);
				count += s->count;
				size += s->total_size;
				time += s->total[4];
				maximum = max(maximum, s->max[4]);
			}
			seq_printf(m, ")");
		}
		/* Stock carries these totals from Read into Write. */
		seq_printf(m, "\t%llu\t%llu\t%llu\t%llu\n", count,
			count ? size / count : 0, count ? time / count : 0, maximum);
		for (cycle = 0; cycle < 2; cycle++) {
			seq_printf(m, "\n%s\n", sample_cycle_config[cycle].name);
			for (range = 0; range < 5; range++) {
				s = &block_info.bucket[rw][cycle][range];
				seq_printf(m, "%s\n", io_range_str[range]);
				seq_printf(m, "blk_section\ttotal_time\tavg_time\tmax_time\tlast_time\tlat_dist\n");
				for (section = 0; section < 5; section++) {
					seq_printf(m, "%s\t%llu\t%llu\t%llu\t%llu\t", block_section_str[section],
						s->total[section], s->avg[section], s->max[section], s->last[section]);
					for (lat = 0; lat < 8; lat++)
						seq_printf(m, "%llu,", s->lat[section][lat]);
					seq_printf(m, "\n");
				}
			}
		}
	}
	spin_unlock_irqrestore(&block_info.lock, flags);
}

static void ufs_show(struct seq_file *m)
{
	unsigned int rw, i, lat;
	struct io_ufs_bucket *s;
	unsigned long flags;
	spin_lock_irqsave(&ufs_info.lock, flags);
	for (rw = 0; rw < 2; rw++) {
		seq_printf(m, "\n%s:\n", rw ? "Write" : "Read");
		seq_printf(m, "cycle\ttotal_cnt\ttotal_size\ttotal_time\tavg_time\tmax_time\tlast_time\tlast_issue_time_stamp\tlast_compl_time_stamp\tlast_size\tlat_dist(,,,)\n");
		for (i = 0; i < 2; i++) {
			s = &ufs_info.bucket[rw][i];
			seq_printf(m, "%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t",
				sample_cycle_config[i].name, s->count, s->total_size, s->total_time, s->avg_time,
				s->max_time, s->last_time, s->issue, s->compl, s->last_size);
			for (lat = 0; lat < 8; lat++)
				seq_printf(m, "%llu,", s->lat[lat]);
			seq_printf(m, "\n");
		}
	}
	spin_unlock_irqrestore(&ufs_info.lock, flags);
}

enum io_group { MAIN, F2FS, BLOCK, UFS, IOSTAT };
struct io_proc_node { const char *name; enum io_group group; umode_t mode; };
static const struct io_proc_node io_monitor_procfs_node[] = {
	{ "enable", MAIN, 0660 }, { "debug_enable", MAIN, 0640 },
	{ "iologbuf_trigger", MAIN, 0660 }, { "reset_stat", MAIN, 0640 },
	{ "injection_enabled", MAIN, 0640 }, { "f2fs_enabled", F2FS, 0640 },
	{ "fsync_thresh", F2FS, 0640 }, { "gc_fg_thresh", F2FS, 0640 },
	{ "cp_thresh", F2FS, 0640 }, { "f2fs_fsync_injection_delay", F2FS, 0640 },
	{ "f2fs_gc_fg_injection_delay", F2FS, 0640 }, { "f2fs_cp_injection_delay", F2FS, 0640 },
	{ "f2fs_fsync_info", F2FS, 0440 }, { "f2fs_gc_info", F2FS, 0440 },
	{ "f2fs_cp_info", F2FS, 0440 }, { "f2fs_read_info", F2FS, 0440 },
	{ "block_enabled", BLOCK, 0640 }, { "block_thresh", BLOCK, 0640 },
	{ "blk_injection_q2g_delay", BLOCK, 0640 }, { "blk_injection_g2i_delay", BLOCK, 0640 },
	{ "blk_injection_i2d_delay", BLOCK, 0640 }, { "blk_injection_d2c_delay", BLOCK, 0640 },
	{ "block_info", BLOCK, 0440 }, { "ufs_enabled", UFS, 0640 },
	{ "ufs_thresh", UFS, 0640 }, { "ufs_injection_delay", UFS, 0640 },
	{ "ufs_info", UFS, 0440 }, { "iostat_enabled", IOSTAT, 0640 },
	{ "iostat_info", IOSTAT, 0440 },
};
static struct proc_dir_entry *io_monitor_procfs[5];
static const char * const io_dir_name[] = { "io_monitor", "f2fs", "block", "ufs", "iostat" };
static const char * const show_name[] = { "io_monitor_main_show", "f2fs_monitor_proc_show",
	"block_monitor_proc_show", "ufs_monitor_proc_show", "iostat_proc_show" };
static const unsigned int show_line[] = { 0x77, 0x289, 0x19e, 0xc9, 0xfe };
static const char * const write_name[] = { "io_monitor_main_write", "f2fs_monitor_proc_write",
	"block_monitor_proc_write", "ufs_monitor_proc_write", "iostat_proc_write" };
static const unsigned int write_line[] = { 0xc5, 0x274, 0x188, 0xb5, 0xec };

static const struct io_proc_node *file_node(struct file *file)
{
	unsigned int i;
	const char *name = file->f_path.dentry->d_name.name;
	for (i = 0; i < ARRAY_SIZE(io_monitor_procfs_node); i++)
		if (!strcmp(name, io_monitor_procfs_node[i].name))
			return &io_monitor_procfs_node[i];
	return NULL;
}

static unsigned int scalar_get(const char *name)
{
#define GET(n, v) if (!strcmp(name, n)) return (v)
	GET("enable", io_monitor_enabled);
	GET("debug_enable", io_monitor_debug_enabled);
	GET("iologbuf_trigger", iologbuf_trigger);
	GET("injection_enabled", io_monitor_injection_enabled);
	GET("f2fs_enabled", f2fs_monitor_enabled);
	GET("fsync_thresh", f2fs_time_threshold[0]);
	GET("gc_fg_thresh", f2fs_time_threshold[1]);
	GET("cp_thresh", f2fs_time_threshold[2]);
	GET("f2fs_fsync_injection_delay", f2fs_injection[0]);
	GET("f2fs_gc_fg_injection_delay", f2fs_injection[1]);
	GET("f2fs_cp_injection_delay", f2fs_injection[2]);
	GET("block_enabled", block_monitor_enabled);
	GET("block_thresh", block_time_threshold);
	GET("blk_injection_q2g_delay", blk_injection[0]);
	GET("blk_injection_g2i_delay", blk_injection[1]);
	GET("blk_injection_i2d_delay", blk_injection[2]);
	GET("blk_injection_d2c_delay", blk_injection[3]);
	GET("ufs_enabled", ufs_monitor_enabled);
	GET("ufs_thresh", ufs_time_threshold);
	GET("ufs_injection_delay", ufs_injection);
	GET("iostat_enabled", iostat_enabled);
#undef GET
	return 0;
}

static int io_monitor_proc_show(struct seq_file *m, void *unused)
{
	const struct io_proc_node *node = file_node(m->private);
	const char *name = node->name;
	if ((node->group == F2FS && (!io_monitor_enabled || !f2fs_monitor_enabled)) ||
	    (node->group == BLOCK && (!io_monitor_enabled || !block_monitor_enabled)) ||
	    (node->group == UFS && (!io_monitor_enabled || !ufs_monitor_enabled)) ||
	    (node->group == IOSTAT && !iostat_enabled)) {
		seq_printf(m, "%s", node->group == BLOCK ? "io_monitor_enabled and block_monitor_enabled hasn't been set to 1\n" :
			node->group == UFS ? "Pls set io_monitor_enabled and ufs_monitor_enabled to 1.\n" :
			node->group == IOSTAT ? "iostat_enabled hasn't been set to 1\n" :
			"io_monitor_enabled hasn't been set to 1\n");
		return -1;
	}
	if (proc_show_enabled || io_monitor_debug_enabled)
		IO_LOG(show_name[node->group], show_line[node->group], "read %s/%s\n", io_dir_name[node->group], name);
	if (!strcmp(name, "reset_stat"))
		return 0;
	if (!strcmp(name, "f2fs_fsync_info"))
		elapsed_show(m, &f2fs_fsync_info);
	else if (!strcmp(name, "f2fs_cp_info"))
		elapsed_show(m, &f2fs_cp_info);
	else if (!strcmp(name, "f2fs_gc_info"))
		gc_show(m);
	else if (!strcmp(name, "f2fs_read_info"))
		f2fs_read_info_output(m, NULL, 0);
	else if (!strcmp(name, "block_info"))
		block_show(m);
	else if (!strcmp(name, "ufs_info"))
		ufs_show(m);
	else if (!strcmp(name, "iostat_info"))
		iostat_output(m, NULL, 0);
	else
		seq_printf(m, "%d\n", scalar_get(name));
	return 0;
}

static int io_monitor_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, io_monitor_proc_show, file);
}

static void reset_stat(void)
{
	f2fs_monitor_reset();
	block_monitor_reset();
	ufs_monitor_reset();
}

static ssize_t io_monitor_proc_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	const struct io_proc_node *node = file_node(file);
	const char *name = node->name;
	char text[64] = { };
	u64 value;
	size_t size;
	int ret;
	bool short_input = node->group == MAIN || node->group == IOSTAT;
	if (short_input && !count)
		return 0;
	size = min_t(size_t, count, short_input ? 63 : 64);
	if (copy_from_user(text, buf, size))
		return -EFAULT;
	/* Stock traps at its bounds check after a successful 64-byte copy. */
	if (!short_input && count >= 64)
		io_proc_bounds_trap();
	text[size] = 0;
	ret = kstrtoull(text, 10, &value);
	if (ret)
		return ret;
	if (!strcmp(name, "enable")) {
		if (value == io_monitor_enabled) {
			IO_LOG("io_monitor_main_write", 0x9d, "enable has been set to %llu\n", value);
			return -EINVAL;
		}
		if (value == 1) {
			if (iologbuf_mm_init() < 0 || f2fs_monitor_mm_init() < 0)
				return -1;
			reset_stat();
			io_monitor_enabled = true;
		} else {
			io_monitor_enabled = false;
			f2fs_monitor_mm_exit();
			iologbuf_mm_exit();
		}
	} else if (!strcmp(name, "iologbuf_trigger")) {
		if (value == 1 && io_monitor_enabled)
			kill_fasync(&io_monitor_async, SIGIO, POLL_IN);
	} else if (!strcmp(name, "reset_stat")) {
		if (value == 1)
			reset_stat();
	} else {
#define SET(n, v) if (!strcmp(name, n)) { (v) = value; goto done; }
#define SET_BOOL(n, v) if (!strcmp(name, n)) { (v) = !!value; goto done; }
		SET_BOOL("debug_enable", io_monitor_debug_enabled);
		SET_BOOL("injection_enabled", io_monitor_injection_enabled);
		SET_BOOL("f2fs_enabled", f2fs_monitor_enabled);
		SET("fsync_thresh", f2fs_time_threshold[0]);
		SET("gc_fg_thresh", f2fs_time_threshold[1]);
		SET("cp_thresh", f2fs_time_threshold[2]);
		SET("f2fs_fsync_injection_delay", f2fs_injection[0]);
		SET("f2fs_gc_fg_injection_delay", f2fs_injection[1]);
		SET("f2fs_cp_injection_delay", f2fs_injection[2]);
		SET_BOOL("block_enabled", block_monitor_enabled);
		SET("block_thresh", block_time_threshold);
		SET("blk_injection_q2g_delay", blk_injection[0]);
		SET("blk_injection_g2i_delay", blk_injection[1]);
		SET("blk_injection_i2d_delay", blk_injection[2]);
		SET("blk_injection_d2c_delay", blk_injection[3]);
		SET_BOOL("ufs_enabled", ufs_monitor_enabled);
		SET("ufs_thresh", ufs_time_threshold);
		SET("ufs_injection_delay", ufs_injection);
		SET_BOOL("iostat_enabled", iostat_enabled);
#undef SET
#undef SET_BOOL
	}
done:
	IO_LOG(write_name[node->group], write_line[node->group], "write %llu to %s/%s\n",
		value, io_dir_name[node->group], name);
	size = short_input ? count : size;
	*ppos += size;
	return size;
}

static const struct proc_ops io_monitor_proc_fops = {
	.proc_open = io_monitor_proc_open,
	.proc_read = seq_read,
	.proc_write = io_monitor_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

int io_monitor_procfs_init(void)
{
	unsigned int i, created = 0, dir;
	io_monitor_procfs[0] = proc_mkdir(io_dir_name[0], NULL);
	if (!io_monitor_procfs[0]) {
		IO_LOG("io_monitor_procfs_init", 0x101, "create io_monitor procfs fails\n");
		return -1;
	}
	for (dir = 1; dir < 5; dir++) {
		io_monitor_procfs[dir] = proc_mkdir(io_dir_name[dir], io_monitor_procfs[0]);
		if (!io_monitor_procfs[dir]) {
			IO_LOG("io_monitor_procfs_init", 0x101 + dir * 7, "create %s procfs fails\n", io_dir_name[dir]);
			goto fail;
		}
	}
	for (i = 0; i < ARRAY_SIZE(io_monitor_procfs_node); i++) {
		const struct io_proc_node *node = &io_monitor_procfs_node[i];
		if (!proc_create(node->name, node->mode, io_monitor_procfs[node->group],
			&io_monitor_proc_fops)) {
			IO_LOG("io_monitor_procfs_init", 0x127, "create %s fails\n", node->name);
			goto fail;
		}
		created++;
	}
	return 0;
fail:
	for (i = 0; i < created; i++)
		remove_proc_entry(io_monitor_procfs_node[i].name,
			io_monitor_procfs[io_monitor_procfs_node[i].group]);
	while (dir > 1) {
		dir--;
		remove_proc_entry(io_dir_name[dir], io_monitor_procfs[0]);
	}
	remove_proc_entry(io_dir_name[0], NULL);
	return -1;
}

void io_monitor_procfs_exit(void)
{
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(io_monitor_procfs_node); i++)
		remove_proc_entry(io_monitor_procfs_node[i].name,
			io_monitor_procfs[io_monitor_procfs_node[i].group]);
	for (i = 4; i > 0; i--)
		remove_proc_entry(io_dir_name[i], io_monitor_procfs[0]);
	remove_proc_entry(io_dir_name[0], NULL);
}

static unsigned int common_line(char *buf, unsigned int off,
		const struct io_common *c, u64 last)
{
	return off + sprintf(buf + off, "\n%llu %llu %llu %d %d %s %d %s %d %s %c\n",
		last, c->timestamp, c->utc_seconds, c->uid, c->pid, c->comm,
		c->ppid, c->pcomm, c->gid, c->group_leader, c->task_state);
}

static unsigned int log_head(char *buf, unsigned int off, unsigned int type)
{
	static const char * const names[] = { "F2FS_FSYNC", "F2FS_GC", "F2FS_CP",
		"F2FS_FILEMAP_READ", "F2FS_FILEMAP_MAP_PAGES", "BLOCK_RQ_COMPLETE", "UFSHCD_COMMAND" };
	const char *fields;
	unsigned int i;
	if (off >= IO_READ_SIZE)
		return off;
	off += scnprintf(buf + off, IO_READ_SIZE - off, "\n%s:\n", names[type]);
	if (off >= IO_READ_SIZE)
		return off;
	off += scnprintf(buf + off, IO_READ_SIZE - off,
		type >= 5 ? "op nr_bytes elapse_time timestamp utc_seconds uid pid comm ppid pcomm tgid group_leader task_state\n" :
		type >= 3 ? "last_size timestamp utc_seconds uid pid comm ppid pcomm tgid group_leader task_state\n" :
		"last_elapse_time timestamp utc_seconds uid pid comm ppid pcomm tgid group_leader task_state\n");
	fields = type == 1 ? "/total_cnt total_seg_cnt total_time avg_time max_time last_time\n" :
		type >= 3 && type <= 4 ? "/total_cnt total_size avg_size max_size last_size\n" :
		type == 5 ? "/total_cnt total_size last_size\nQ2G/total avg max last lat_range(0~100us,100~200us,200~500us,500us~2ms,2~20ms,20~100ms,100~500ms,500~Max\nG2I/total avg max last lat_range(...)\nI2D/total avg max last lat_range(...)\nD2C/total avg max last lat_range(...)\nQ2C/total avg max last lat_range(...)\n" :
		type == 6 ? "/total_cnt total_size last_size\ntotal avg max last(elapse,issue,compl) lat_range(0~100us,100~200us,200~500us,500us~2ms,2~20ms,20~100ms,100~500ms,500~Max\n" :
		"/total_cnt total_time avg_time max_time last_time\n";
	for (i = 0; i < 2 && off < IO_READ_SIZE; i++) {
		if (type < 3)
			off += scnprintf(buf + off, IO_READ_SIZE - off, "%.*s%.*s", 64,
				sample_cycle_config[i].name, 128, fields);
		else
			off += scnprintf(buf + off, IO_READ_SIZE - off, "%s%s", sample_cycle_config[i].name, fields);
	}
	return off;
}

static unsigned int log_write(char *buf, unsigned int off, const struct io_log *log)
{
	unsigned int i, j, k;
	const struct io_common *c;
	switch (log->type) {
	case 0:
	case 2:
		off = common_line(buf, off, &log->data.elapsed.common, log->data.elapsed.last);
		for (i = 0; i < 2; i++) {
			const struct io_elapsed *s = &log->data.elapsed.cycle[i];
			off += sprintf(buf + off, "%llu %llu %llu %llu %llu\n", s->count, s->total, s->avg, s->max, s->last);
		}
		break;
	case 1:
		off = common_line(buf, off, &log->data.gc.common, log->data.gc.last);
		for (i = 0; i < 2; i++) {
			const struct io_gc_elapsed *s = &log->data.gc.cycle[i];
			off += sprintf(buf + off, "%llu %llu %llu %llu %llu %llu\n",
				s->count, s->segments, s->total, s->avg, s->max, s->last);
		}
		break;
	case 3:
	case 4:
		off = common_line(buf, off, &log->data.filemap.common, log->data.filemap.last);
		for (i = 0; i < 2; i++) {
			const struct io_size_stat *s = &log->data.filemap.cycle[i];
			off += sprintf(buf + off, "%llu %llu %llu %llu %llu\n", s->count, s->total, s->avg, s->max, s->last);
		}
		break;
	case 5:
	case 6:
		c = log->type == 5 ? &log->data.block.common : &log->data.ufs.common;
		off += sprintf(buf + off, "\n%s %llu %llu %llu %llu %d %d %s %d %s %d %s %c\n",
			(log->type == 5 ? log->data.block.rw : log->data.ufs.rw) ? "write" : "read",
			log->type == 5 ? log->data.block.size : log->data.ufs.size,
			log->type == 5 ? log->data.block.last : log->data.ufs.last,
			c->timestamp, c->utc_seconds, c->uid, c->pid, c->comm, c->ppid, c->pcomm,
			c->gid, c->group_leader, c->task_state);
		for (i = 0; i < 2; i++) {
			if (log->type == 5) {
				const struct io_block_bucket *s = &log->data.block.cycle[i];
				off += sprintf(buf + off, "%llu %llu %llu\n", s->count, s->total_size, s->last_size);
				for (j = 0; j < 5; j++) {
					off += sprintf(buf + off, "%llu %llu %llu %llu ", s->total[j], s->avg[j], s->max[j], s->last[j]);
					if (off >= IO_READ_SIZE - 1)
						io_output_bounds_trap();
					off += sprintf(buf + off, "(");
					for (k = 0; k < 8; k++)
						off += sprintf(buf + off, "%llu,", s->lat[j][k]);
					off += sprintf(buf + off, ")\n");
				}
			} else {
				const struct io_ufs_bucket *s = &log->data.ufs.cycle[i];
				off += sprintf(buf + off, "%llu %llu %llu\n%llu %llu %llu %llu %llu %llu ",
					s->count, s->total_size, s->last_size, s->total_time, s->avg_time, s->max_time,
					s->last_time, s->issue, s->compl);
				if (off >= IO_READ_SIZE - 1)
					io_output_bounds_trap();
				off += sprintf(buf + off, "(");
				for (k = 0; k < 8; k++)
					off += sprintf(buf + off, "%llu,", s->lat[k]);
				off += sprintf(buf + off, ")\n");
			}
		}
		break;
	}
	return off;
}

/* .text+0x646c..0x6eec: count/ppos ignored, newest-first, temporary global disable. */
static ssize_t io_monitor_dev_read(struct file *file, char __user *user,
		size_t count, loff_t *ppos)
{
	char *buf;
	unsigned int off = 0, index, i, logs;
	ssize_t ret;
	buf = kvzalloc(IO_READ_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (!io_monitor_enabled || (!f2fs_monitor_enabled && !block_monitor_enabled &&
		!ufs_monitor_enabled && !iostat_enabled)) {
		__copy_overflow(IO_READ_SIZE, (unsigned long)-1);
		ret = -EFAULT;
		goto out;
	}
	io_monitor_enabled = false;
	logs = __io_monitor_log_ctrl.count;
	index = __io_monitor_log_ctrl.index;
	IO_LOG("copy_log_to_buf", 0x163, "log_count: %d, log_index: %d\n", logs, index);
	for (i = 0; i < logs; i++) {
		const struct io_log *log;
		index = (index ? index : logs) - 1;
		log = &__io_monitor_log_ctrl.buf[index];
		if (log->type > 6) {
			IO_LOG("write_head_to_buf", 0xb0, "unknown io monitor type: %d\n", log->type);
			IO_LOG("write_log_to_buf", 0x142, "unknown io monitor type: %d\n", log->type);
			continue;
		}
		off = log_head(buf, off, log->type);
		off = log_write(buf, off, log);
	}
	off = f2fs_read_info_output(NULL, buf, off);
	off = iostat_output(NULL, buf, off);
	io_monitor_enabled = true;
	ret = off;
	if (off > IO_READ_SIZE) {
		__copy_overflow(IO_READ_SIZE, off);
		ret = -EFAULT;
	} else if (!off || copy_to_user(user, buf, off))
		ret = -EFAULT;
out:
	kvfree(buf);
	return ret;
}

static int io_monitor_dev_open(struct inode *inode, struct file *file) { return 0; }
static int io_monitor_dev_release(struct inode *inode, struct file *file) { return 0; }
static int io_monitor_dev_fasync(int fd, struct file *file, int on)
{
	return fasync_helper(fd, file, on, &io_monitor_async);
}
static const struct file_operations io_monitor_dev_operations = {
	.owner = THIS_MODULE,
	.read = io_monitor_dev_read,
	.open = io_monitor_dev_open,
	.release = io_monitor_dev_release,
	.fasync = io_monitor_dev_fasync,
};
static struct miscdevice io_monitor_miscdevice = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "io_monitor",
	.fops = &io_monitor_dev_operations,
};
int iologbuf_init(void)
{
	int ret = misc_register(&io_monitor_miscdevice);
	if (!ret)
		IO_LOG("iologbuf_init", 0x2ed, "%s ok\n", "iologbuf_init");
	return ret;
}

void iologbuf_exit(void)
{
	misc_deregister(&io_monitor_miscdevice);
	IO_LOG("iologbuf_exit", 0x2f6, "%s\n", "iologbuf_exit");
}
