// SPDX-License-Identifier: GPL-2.0
/* Recovered from stock io_monitor.ko; see RECONSTRUCTION.txt. */
#include <linux/blk-mq.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/tracepoint.h>
#include <scsi/scsi_cmnd.h>
#include <ufs/ufshcd.h>
#include "io_monitor.h"

bool io_monitor_enabled, io_monitor_debug_enabled, io_monitor_injection_enabled;
bool iologbuf_trigger;
bool proc_show_enabled = true;
bool block_monitor_enabled = true, f2fs_monitor_enabled = true;
bool ufs_monitor_enabled = true, iostat_enabled = true;
module_param(proc_show_enabled, bool, 0644);
module_param(block_monitor_enabled, bool, 0644);
module_param(f2fs_monitor_enabled, bool, 0644);
module_param(ufs_monitor_enabled, bool, 0644);
module_param(iostat_enabled, bool, 0644);
MODULE_PARM_DESC(proc_show_enabled, "Debug proc");
MODULE_PARM_DESC(block_monitor_enabled, "block monitor enable");
MODULE_PARM_DESC(f2fs_monitor_enabled, "f2fs monitor enable");
MODULE_PARM_DESC(ufs_monitor_enabled, "ufs monitor enable");
MODULE_PARM_DESC(iostat_enabled, "iostat monitor enable");

int block_time_threshold = 5, f2fs_time_threshold[3] = { 2000, 30000, 4000 };
int ufs_time_threshold = 10;
int blk_injection[4], f2fs_injection[3], ufs_injection;
unsigned int f2fs_filemap_read_notify, f2fs_filemap_map_pages_notify;
struct io_cycle sample_cycle_config[2] = {
	{ 0, "second_10", 10000000000ULL },
	{ 1, "day", 86400000000000ULL },
};
struct io_fsync f2fs_fsync_info, f2fs_cp_info;
struct io_gc f2fs_gc_info;
struct io_filemap_ctrl f2fs_read_info;
struct io_block block_info;
struct io_ufs ufs_info;
struct io_ring_info __io_monitor_info_ctrl;
struct io_ring_log __io_monitor_log_ctrl;
struct io_iostat iostat_info;
struct fasync_struct *io_monitor_async;
static DEFINE_SPINLOCK(io_monitor_info_spinlock);
static DEFINE_SPINLOCK(io_monitor_log_spinlock);
static u64 f2fs_monitor_timestamp[2], ufs_monitor_timestamp[2];
static unsigned int gc_tp;
static int total_alloc_mem;

void cal_total_mem(int bytes)
{
	total_alloc_mem += bytes;
}

long io_ms_to_ns(long ms)
{
	return ms * 1000000;
}

unsigned long io_ns_to_ms(unsigned long ns)
{
	return ns / 1000000;
}

/* Stock get_common_info .text+0x8e8..0x9e8. uid/gid are cred fields. */
void get_common_info(struct io_common *c)
{
	struct task_struct *parent;
	c->timestamp = ktime_get_ns();
	c->utc_seconds = ktime_get_real_seconds();
	strncpy(c->comm, current->comm, 16);
	c->uid = __kuid_val(current_uid());
	c->gid = __kgid_val(current_gid());
	c->pid = current->pid;
	rcu_read_lock();
	if (current->thread_pid) {
		parent = rcu_dereference(current->real_parent);
		c->ppid = parent->pid;
		strncpy(c->pcomm, parent->comm, 16);
	}
	rcu_read_unlock();
	c->task_state = task_state_to_char(current);
	if (current->group_leader)
		strncpy(c->group_leader, current->group_leader->comm, 16);
}

/* Stock .text+0x84c..0x8e4: signed delay, global one-shot flag. */
void io_monitor_injection(int *delay)
{
	u64 n;
	int ms;
	if (!io_monitor_enabled || !io_monitor_injection_enabled || !*delay)
		return;
	ms = *delay;
	n = (s64)ms;
	io_monitor_injection_enabled = false;
	do {
		n--;
		udelay(1000);
	} while (n);
	IO_LOG("io_monitor_injection", 0x34, "mdelay %d\n", ms);
	*delay = 0;
}

int iologbuf_mm_init(void)
{
	__io_monitor_info_ctrl.buf = kvzalloc(IO_RING_SIZE, GFP_KERNEL);
	if (!__io_monitor_info_ctrl.buf) {
		IO_LOG("iologbuf_mm_init", 0x291, "kvzalloc io_monitor_info_ctrl buf failed!\n");
		return -1;
	}
	__io_monitor_log_ctrl.buf = kvzalloc(IO_RING_SIZE, GFP_KERNEL);
	if (!__io_monitor_log_ctrl.buf) {
		IO_LOG("iologbuf_mm_init", 0x298, "kvzalloc io_monitor_log_ctrl buf failed!\n");
		kvfree(__io_monitor_info_ctrl.buf);
		return -1;
	}
	return 0;
}

/* Stock drops pointers without freeing either ring (.text+0x6448..0x6468). */
void iologbuf_mm_exit(void)
{
	memset(&__io_monitor_info_ctrl, 0, sizeof(__io_monitor_info_ctrl));
	memset(&__io_monitor_log_ctrl, 0, sizeof(__io_monitor_log_ctrl));
}

int f2fs_monitor_mm_init(void)
{
	f2fs_read_info.info = kvzalloc(0xd00000, GFP_KERNEL);
	if (!f2fs_read_info.info) {
		IO_LOG("f2fs_monitor_mm_init", 0x2e3, "f2fs read buf kvzalloc failed!\n");
		return -1;
	}
	f2fs_filemap_read_notify = 0;
	f2fs_filemap_map_pages_notify = 0;
	return 0;
}

/* Retain the dangling pointer left by the stock free. */
void f2fs_monitor_mm_exit(void)
{
	if (f2fs_read_info.info)
		kvfree(f2fs_read_info.info);
}

void f2fs_monitor_reset(void)
{
	memset(f2fs_monitor_timestamp, 0, sizeof(f2fs_monitor_timestamp));
	memset(&f2fs_fsync_info, 0, sizeof(f2fs_fsync_info));
	memset(&f2fs_gc_info, 0, sizeof(f2fs_gc_info));
	memset(&f2fs_cp_info, 0, sizeof(f2fs_cp_info));
}

void block_monitor_reset(void)
{
	memset(&block_info, 0, sizeof(block_info));
}

void ufs_monitor_reset(void)
{
	memset(ufs_monitor_timestamp, 0, sizeof(ufs_monitor_timestamp));
	memset(&ufs_info, 0, sizeof(ufs_info));
}

/* .text+0x5e88..0x6374: ring reservation is locked, filling is not. */
static struct xiaomi_io_monitor_event *io_monitor_global_info(unsigned int action, u64 sector,
		unsigned int bytes, unsigned int parent, u64 now, unsigned int rw)
{
	struct xiaomi_io_monitor_event *old = NULL, *event;
	unsigned int index = __io_monitor_info_ctrl.index;
	unsigned int left = min(__io_monitor_info_ctrl.count, 5000U);
	unsigned int same = 64, i;
	u64 delay = 0;
	unsigned long flags;
	static const char * const action_name[] = { "0", "bio_queue", "getrq", "rq_insert", "rq_issue", "rq_complete" };
	if (sector == (u64)-1)
		sector = 0;
	if (parent && sector) {
		if (action <= parent)
			IO_LOG("io_monitor_info_find_index", 0x21d, "[IO_MONITOR] %s\n", "action > parent");
		while (left && same && __io_monitor_info_ctrl.count) {
			index = (index ? index : __io_monitor_info_ctrl.count) - 1;
			event = &__io_monitor_info_ctrl.buf[index];
			if (event->action == parent && event->sector == sector &&
			    event->nr_bytes == bytes && event->rw == rw) {
				old = event;
				delay = now - old->begin[parent];
				break;
			}
			same -= event->action == parent;
			left--;
		}
		if (io_monitor_debug_enabled)
			IO_LOG("io_monitor_global_info", 0x25a,
				"action: %s parent: %s nr_bytes: %d sector: %lu index: %d delay: %llu\n",
				action_name[action], action_name[parent], bytes, (unsigned long)sector,
				old ? index : (unsigned int)-1, delay);
	}
	spin_lock_irqsave(&io_monitor_info_spinlock, flags);
	event = &__io_monitor_info_ctrl.buf[__io_monitor_info_ctrl.index];
	if (++__io_monitor_info_ctrl.index >= IO_INFO_MAX)
		__io_monitor_info_ctrl.index = 0;
	if (__io_monitor_info_ctrl.count < IO_INFO_MAX)
		__io_monitor_info_ctrl.count++;
	spin_unlock_irqrestore(&io_monitor_info_spinlock, flags);
	memset(event, 0, sizeof(*event));
	if (action == 5 && parent == 4 && old && delay)
		event->common = old->common;
	else if (action == 4)
		get_common_info(&event->common);
	event->action = action;
	event->rw = rw;
	event->nr_bytes = bytes;
	event->sector = sector;
	if (parent && old && delay) {
		for (i = parent; i > 0; i--) {
			event->begin[i] = old->begin[i];
			event->delay[i] = old->delay[i];
		}
	}
	event->begin[action] = now;
	event->delay[action] = delay;
	if (parent && old)
		memset(old, 0, sizeof(*old));
	if (io_monitor_debug_enabled)
		IO_LOG("io_monitor_global_info", 0x284,
			"action: %s nr_bytes: %d sector: %lu begin_time:[%llu %llu %llu %llu %llu] delay:[%llu %llu %llu %llu %llu]\n",
			action_name[action], bytes, (unsigned long)sector, event->begin[1], event->begin[2],
			event->begin[3], event->begin[4], event->begin[5], event->delay[1],
			event->delay[2], event->delay[3], event->delay[4], event->delay[5]);
	return event;
}

/* .text+0x5b38..0x5e84: publish type/index before copying snapshot data. */
void io_monitor_global_log(void *info, int type)
{
	struct io_log *log;
	struct io_fsync *elapsed = info;
	struct io_gc *gc = info;
	struct io_filemap *filemap = info;
	struct io_block *block = info;
	struct io_ufs *ufs = info;
	unsigned int i, kind;
	unsigned long flags;
	spin_lock_irqsave(&io_monitor_log_spinlock, flags);
	log = &__io_monitor_log_ctrl.buf[__io_monitor_log_ctrl.index];
	log->type = type;
	if (++__io_monitor_log_ctrl.index >= IO_LOG_MAX)
		__io_monitor_log_ctrl.index = 0;
	if (__io_monitor_log_ctrl.count < IO_LOG_MAX)
		__io_monitor_log_ctrl.count++;
	spin_unlock_irqrestore(&io_monitor_log_spinlock, flags);
	if (!info)
		return;
	switch (type) {
	case 0:
	case 2:
		get_common_info(&log->data.elapsed.common);
		log->data.elapsed.last = elapsed->cycle[1].last;
		memcpy(log->data.elapsed.cycle, elapsed->cycle, sizeof(elapsed->cycle));
		break;
	case 1:
		get_common_info(&log->data.gc.common);
		log->data.gc.last = gc->cycle[1][gc->type].last;
		for (i = 0; i < 2; i++)
			log->data.gc.cycle[i] = gc->cycle[i][gc->type];
		break;
	case 3:
	case 4:
		kind = type != 3;
		log->data.filemap.task_type = filemap->task_type;
		log->data.filemap.common = filemap->common;
		for (i = 0; i < 2; i++)
			log->data.filemap.cycle[i] = filemap->stats[filemap->task_type][kind][i];
		log->data.filemap.last = log->data.filemap.cycle[1].last;
		break;
	case 5:
		log->data.block.common = block->common;
		log->data.block.rw = block->rw;
		log->data.block.range = block->range;
		for (i = 0; i < 2; i++)
			log->data.block.cycle[i] = block->bucket[block->rw][i][block->range];
		log->data.block.last = log->data.block.cycle[1].last[4];
		log->data.block.size = log->data.block.cycle[1].last_size;
		break;
	case 6:
		get_common_info(&log->data.ufs.common);
		log->data.ufs.rw = ufs->rw;
		for (i = 0; i < 2; i++)
			log->data.ufs.cycle[i] = ufs->bucket[ufs->rw][i];
		log->data.ufs.last = log->data.ufs.cycle[1].last_time;
		log->data.ufs.size = log->data.ufs.cycle[1].last_size;
		break;
	default:
		IO_LOG("io_monitor_global_log", 0x1fc, "unknown io monitor type: %d\n", type);
	}
}

static unsigned int latency_range(u64 ns)
{
	static const u64 limits[] = { 100000, 200000, 500000, 2000000,
		20000000, 100000000, 500000000 };
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(limits); i++)
		if (ns <= limits[i])
			return i;
	return 7;
}

static void cb_block_bio_queue(void *unused, struct bio *bio)
{
	unsigned int rw = bio_op(bio);
	u64 now;
	if (io_monitor_enabled && block_monitor_enabled && bio->bi_iter.bi_size && rw < 2) {
		now = ktime_get_ns();
		io_monitor_global_info(1, bio->bi_iter.bi_sector, bio->bi_iter.bi_size,
			0, now, rw);
		if (io_monitor_debug_enabled)
			IO_LOG("cb_block_bio_queue", 0x46, "%s size: %u bi_sector:%llu current_time:%llu\n",
				rw ? "write" : "read", bio->bi_iter.bi_size, bio->bi_iter.bi_sector, now);
	}
}

static void cb_block_getrq(void *unused, struct bio *bio)
{
	unsigned int rw = bio_op(bio);
	u64 now;
	if (io_monitor_enabled && block_monitor_enabled && bio->bi_iter.bi_size && rw < 2) {
		io_monitor_injection(&blk_injection[0]);
		now = ktime_get_ns();
		io_monitor_global_info(2, bio->bi_iter.bi_sector, bio->bi_iter.bi_size,
			1, now, rw);
		if (io_monitor_debug_enabled)
			IO_LOG("cb_block_getrq", 0x67, "%s size: %u bi_sector:%llu current_time:%llu\n",
				rw ? "write" : "read", bio->bi_iter.bi_size, bio->bi_iter.bi_sector, now);
	}
}

static void cb_block_rq_insert(void *unused, struct request *rq)
{
	u64 now;
	if (io_monitor_enabled && block_monitor_enabled && blk_rq_bytes(rq) && req_op(rq) < 2) {
		io_monitor_injection(&blk_injection[1]);
		now = ktime_get_ns();
		io_monitor_global_info(3, blk_rq_pos(rq), blk_rq_bytes(rq), 2,
			now, req_op(rq));
		if (io_monitor_debug_enabled)
			IO_LOG("cb_block_rq_insert", 0x88, "%s size: %u __sector:%llu current_time_ns:%llu\n",
				req_op(rq) ? "write" : "read", blk_rq_bytes(rq), blk_rq_pos(rq), now);
	}
}

static void cb_block_rq_issue(void *unused, struct request *rq)
{
	u64 now;
	if (io_monitor_enabled && block_monitor_enabled && blk_rq_bytes(rq) && req_op(rq) < 2) {
		io_monitor_injection(&blk_injection[2]);
		rq->io_start_time_ns = ktime_get_ns();
		now = ktime_get_ns();
		io_monitor_global_info(4, blk_rq_pos(rq), blk_rq_bytes(rq), 3,
			now, req_op(rq));
		if (io_monitor_debug_enabled)
			IO_LOG("cb_block_rq_issue", 0xaa, "%s size: %u __sector:%llu current_time:%llu\n",
				req_op(rq) ? "write" : "read", blk_rq_bytes(rq), blk_rq_pos(rq), now);
	}
}

/* .text+0xe3c..0x2364: independently sampled size/direction buckets. */
static void cb_block_rq_complete(void *unused, struct request *rq,
		blk_status_t status, unsigned int bytes)
{
	struct xiaomi_io_monitor_event *event;
	struct io_block_bucket *bucket;
	u64 now, diff, delay[5], threshold;
	unsigned int rw = req_op(rq), range, i, j;
	unsigned long flags;
	if (!bytes || !io_monitor_enabled || !block_monitor_enabled || rw >= 2)
		return;
	threshold = (s64)block_time_threshold;
	io_monitor_injection(&blk_injection[3]);
	now = ktime_get_ns();
	event = io_monitor_global_info(5, blk_rq_pos(rq), bytes, 4, now, rw);
	for (i = 1; i < 6; i++)
		if (!event->begin[i])
			return;
	range = bytes >= 524288 ? 4 : bytes > 131072 ? 3 : bytes > 32768 ? 2 : bytes > 4096;
	spin_lock_irqsave(&block_info.lock, flags);
	block_info.common = event->common;
	block_info.rw = rw;
	block_info.range = range;
	memset(block_info.reserved, 0, sizeof(block_info.reserved));
	block_info.nr_bytes = bytes;
	for (j = 0; j < 4; j++)
		delay[j] = event->delay[j + 2];
	delay[4] = now - event->begin[1];
	for (i = 0; i < 2; i++) {
		bucket = &block_info.bucket[rw][i][range];
		diff = now - bucket->timestamp;
		bucket->rw = rw;
		bucket->last_size = bytes;
		if (diff < now) {
			bucket->count++;
			bucket->total_size += bytes;
			for (j = 0; j < 5; j++) {
				bucket->total[j] += delay[j];
				bucket->avg[j] = bucket->count ? bucket->total[j] / bucket->count : 0;
				bucket->max[j] = max(bucket->max[j], delay[j]);
				bucket->last[j] = delay[j];
				bucket->lat[j][latency_range(delay[j])]++;
			}
		} else {
			diff = 0;
			bucket->timestamp = now;
			bucket->count = 1;
			bucket->total_size = bytes;
			for (j = 0; j < 5; j++) {
				bucket->total[j] = delay[j];
				bucket->avg[j] = delay[j];
				bucket->max[j] = delay[j];
				bucket->last[j] = delay[j];
			}
			memset(bucket->lat, 0, sizeof(bucket->lat));
			for (j = 0; j < 5; j++)
				bucket->lat[j][latency_range(delay[j])] = 1;
		}
		if (diff >= sample_cycle_config[i].ns)
			bucket->timestamp = 0;
	}
	if (block_info.bucket[rw][1][range].last[4] / 1000000 > threshold) {
		io_monitor_global_log(&block_info, 5);
		f2fs_filemap_read_notify = 1;
		f2fs_filemap_map_pages_notify = 1;
		if (io_monitor_debug_enabled)
			IO_LOG("cb_block_rq_complete", 0x12e, "Overtime: %llu\n", delay[4] / 1000000);
	}
	spin_unlock_irqrestore(&block_info.lock, flags);
	if (io_monitor_debug_enabled)
		IO_LOG("cb_block_rq_complete", 0x14a,
			"%s %d range(%d) __sector:%llu elapse[Q2G]:%llu elapse[G2I]:%llu elapse[I2D]:%llu elapse[D2C]:%llu elapse[Q2C]:%llu begin_time[BLOCK_BIO_QUEUE_TAG]:%llu begin_time[BLOCK_GETRQ_TAG]:%llu begin_time[BLOCK_RQ_INSERT_TAG]:%llu begin_time[BLOCK_RQ_ISSUE_TAG]:%llu begin_time[BLOCK_RQ_COMPLETE_TAG]:%llu current_time:%llu\n",
			rw ? "write" : "read", bytes, range, blk_rq_pos(rq), event->delay[2],
			event->delay[3], event->delay[4], event->delay[5], now - event->begin[1],
			event->begin[1], event->begin[2], event->begin[3], event->begin[4], event->begin[5], now);
}

/* These timestamps are shared by all F2FS and filemap operations in stock. */
static void elapsed_begin(struct io_fsync *info, u64 now, const char *msg)
{
	unsigned int i;
	u64 diff;
	for (i = 0; i < 2; i++) {
		diff = now - f2fs_monitor_timestamp[i];
		if (now <= diff) {
			memset(&info->cycle[i], 0, 5 * sizeof(u64));
			f2fs_monitor_timestamp[i] = now;
			diff = 0;
		}
		if (diff >= sample_cycle_config[i].ns)
			f2fs_monitor_timestamp[i] = 0;
		info->cycle[i].begin = now;
		if (msg && io_monitor_debug_enabled) {
			struct io_elapsed *s = &info->cycle[i];
			IO_LOG("cb_f2fs_write_checkpoint", 0xd8,
				"%s cycle(%d) total_cnt:%llu total_elapse_time:%llu avg_elapse_time:%llu max_elapse_time:%llu last_elapse_time:%llu begin_time:%llu current_time:%llu\n",
				msg, i, s->count, s->total, s->avg, s->max, s->last, s->begin, now);
		}
	}
}

static u64 elapsed_end(struct io_fsync *info, u64 now, const char *msg)
{
	unsigned int i;
	u64 ns = 0;
	for (i = 0; i < 2; i++) {
		struct io_elapsed *s = &info->cycle[i];
		if (s->begin) {
			ns = now - s->begin;
			s->count++;
			s->total += ns;
			s->avg = s->count ? s->total / s->count : 0;
			s->max = max(s->max, ns);
			s->last = ns;
		}
		if (io_monitor_debug_enabled && (msg || s->begin)) {
			if (msg)
				IO_LOG("cb_f2fs_write_checkpoint", 0xf3,
					"%s cycle(%d) total_cnt:%llu total_elapse_time:%llu avg_elapse_time:%llu max_elapse_time:%llu last_elapse_time:%llu begin_time:%llu current_time:%llu\n",
					msg, i, s->count, s->total, s->avg, s->max, s->last, s->begin, now);
			else
				IO_LOG("cb_f2fs_sync_file_exit", 0x14f,
					"fsync cycle(%d): total_cnt:%llu total_time:%llu avg_time:%llu max_time:%llu last_time:%llu begin_time:%llu current_time:%llu\n",
					i, s->count, s->total, s->avg, s->max, s->last, s->begin, now);
		}
		if (msg)
			s->begin = 0;
	}
	return ns;
}

static void cb_f2fs_sync_file_enter(void *unused, struct inode *inode)
{
	u64 now;
	if (!io_monitor_enabled || !f2fs_monitor_enabled)
		return;
	spin_lock(&f2fs_fsync_info.lock);
	now = ktime_get_ns();
	elapsed_begin(&f2fs_fsync_info, now, NULL);
	spin_unlock(&f2fs_fsync_info.lock);
	if (io_monitor_debug_enabled)
		IO_LOG("cb_f2fs_sync_file_enter", 0x126, "current_time_ns:%llu\n", now);
}

static void cb_f2fs_sync_file_exit(void *unused, struct inode *inode,
		int cp_reason, int datasync, int ret)
{
	u64 ns, now, threshold;
	if (!io_monitor_enabled || !f2fs_monitor_enabled)
		return;
	threshold = (s64)f2fs_time_threshold[0];
	io_monitor_injection(&f2fs_injection[0]);
	spin_lock(&f2fs_fsync_info.lock);
	/* Stock deliberately does not clear the fsync begin timestamps. */
	now = ktime_get_ns();
	ns = elapsed_end(&f2fs_fsync_info, now, NULL);
	if (ns / 1000000 > threshold) {
		io_monitor_global_log(&f2fs_fsync_info, 0);
		if (io_monitor_debug_enabled)
			IO_LOG("cb_f2fs_sync_file_exit", 0x157, "Overtime:%llu\n", ns);
	}
	spin_unlock(&f2fs_fsync_info.lock);
}

static void cb_f2fs_write_checkpoint(void *unused, struct super_block *sb,
		int reason, const char *msg)
{
	u64 now, ns, threshold;
	if (!io_monitor_enabled || !f2fs_monitor_enabled)
		return;
	threshold = (s64)f2fs_time_threshold[2];
	spin_lock(&f2fs_cp_info.lock);
	now = ktime_get_ns();
	if (!strcmp(msg, "start block_ops")) {
		io_monitor_injection(&f2fs_injection[2]);
		elapsed_begin(&f2fs_cp_info, now, msg);
	} else if (!strcmp(msg, "finish checkpoint")) {
		ns = elapsed_end(&f2fs_cp_info, now, msg);
		if (ns / 1000000 > threshold) {
			io_monitor_global_log(&f2fs_cp_info, 2);
			if (io_monitor_debug_enabled)
				IO_LOG("cb_f2fs_write_checkpoint", 0xfc, "Overtime:%llu\n", ns);
		}
	} else {
		goto unlock;
	}
unlock:
	spin_unlock(&f2fs_cp_info.lock);
}

static void cb_f2fs_gc_begin(void *unused, struct super_block *sb,
		int gc_type, bool no_bg_gc, unsigned int nr_free_secs,
		long long dirty_nodes, long long dirty_dents, long long dirty_imeta,
		unsigned int free_sec, unsigned int free_seg, int reserved_seg,
		unsigned int prefree_seg)
{
	u64 now, diff;
	unsigned int i;
	if (!io_monitor_enabled || !f2fs_monitor_enabled)
		return;
	spin_lock(&f2fs_gc_info.lock);
	now = ktime_get_ns();
	/* Stock x3 is no_bg_gc, not gc_type. */
	gc_tp = no_bg_gc & 1;
	for (i = 0; i < 2; i++) {
		diff = now - f2fs_monitor_timestamp[i];
		if (now <= diff) {
			memset(&f2fs_gc_info.cycle[i][gc_tp], 0, 6 * sizeof(u64));
			f2fs_monitor_timestamp[i] = now;
			diff = 0;
		}
		f2fs_gc_info.cycle[i][gc_tp].begin = now;
		if (diff >= sample_cycle_config[i].ns)
			f2fs_monitor_timestamp[i] = 0;
	}
	spin_unlock(&f2fs_gc_info.lock);
	if (io_monitor_debug_enabled)
		IO_LOG("cb_f2fs_gc_begin", 100, "current_time_ns:%llu\n", now);
}

static void cb_f2fs_gc_end(void *unused, struct super_block *sb, int ret,
		int seg_freed, int sec_freed, long long dirty_nodes,
		long long dirty_dents, long long dirty_imeta, unsigned int free_sec,
		unsigned int free_seg, int reserved_seg, unsigned int prefree_seg)
{
	u64 now, threshold, ns = 0;
	unsigned int i;
	if (!io_monitor_enabled || !f2fs_monitor_enabled || !(sb->s_flags & SB_ACTIVE))
		return;
	threshold = (s64)f2fs_time_threshold[1];
	io_monitor_injection(&f2fs_injection[1]);
	spin_lock(&f2fs_gc_info.lock);
	now = ktime_get_ns();
	f2fs_gc_info.type = gc_tp;
	for (i = 0; i < 2; i++) {
		struct io_gc_elapsed *s = &f2fs_gc_info.cycle[i][gc_tp];
		if (s->begin) {
			ns = now - s->begin;
			s->begin = 0;
			s->count++;
			/* Stock stack+8 uses free_seg, not seg_freed. */
			s->segments += free_seg;
			s->total += ns;
			s->avg = s->count ? s->total / s->count : 0;
			s->max = max(s->max, ns);
			s->last = ns;
		}
		if (io_monitor_debug_enabled)
			IO_LOG("cb_f2fs_gc_end", 0x9b,
				"%s cycle[%d] gc_thresh:%d gc_elapse:%llu begin_time:%llu current_time:%llu\n",
				gc_tp ? "FG" : "BG", i, (int)threshold, ns, s->begin, now);
	}
	if (ns / 1000000 > threshold) {
		io_monitor_global_log(&f2fs_gc_info, 1);
		if (io_monitor_debug_enabled)
			IO_LOG("cb_f2fs_gc_end", 0xa2, "Overtime:%llu\n", ns);
	}
	spin_unlock(&f2fs_gc_info.lock);
}

static void filemap_record(unsigned int kind, u64 size)
{
	struct io_common common = { };
	struct io_filemap *info;
	unsigned int task_type, i;
	u64 now, diff;
	spinlock_t *lock = kind ? &f2fs_read_info.map_lock : &f2fs_read_info.read_lock;
	if (!size || !io_monitor_enabled || !f2fs_monitor_enabled)
		return;
	get_common_info(&common);
	if (common.pid >= IO_PID_MAX || common.ppid >= IO_PID_MAX)
		return;
	if (!strcmp(common.comm, "main_thread"))
		task_type = 0;
	else if (!strcmp(common.comm, "RenderThread"))
		task_type = 1;
	else
		return;
	spin_lock(lock);
	info = &f2fs_read_info.info[common.ppid];
	info->valid = true;
	info->task_type = task_type;
	info->common = common;
	now = ktime_get_ns();
	for (i = 0; i < 2; i++) {
		struct io_size_stat *s = &info->stats[task_type][kind][i];
		diff = now - f2fs_monitor_timestamp[i];
		if (diff < now) {
			s->count++;
			s->total += size;
			s->avg = s->count ? s->total / s->count : 0;
			s->max = max(s->max, size);
		} else {
			diff = 0;
			f2fs_monitor_timestamp[i] = now;
			s->count = 1;
			s->total = size;
			s->avg = size;
			s->max = size;
		}
		/* Stock read hook never writes last_size. */
		if (kind)
			s->last = size;
		if (diff >= sample_cycle_config[i].ns)
			f2fs_monitor_timestamp[i] = 0;
		if (io_monitor_debug_enabled)
			IO_LOG(kind ? "cb_filemap_map_pages" : "cb_filemap_read", kind ? 0x1dc : 0x194,
				"cycle(%d) total_cnt:%llu total_size:%llu avg_size:%llu max_size:%llu current_time_ns:%llu\n",
				i, s->count, s->total, s->avg, s->max, now);
	}
	if (kind ? f2fs_filemap_map_pages_notify == 1 : f2fs_filemap_read_notify == 1) {
		if (kind)
			f2fs_filemap_map_pages_notify = 0;
		else
			f2fs_filemap_read_notify = 0;
		io_monitor_global_log(info, kind ? 4 : 3);
	}
	spin_unlock(lock);
}

static void cb_filemap_read(void *unused, struct file *file, loff_t pos, size_t size)
{
	filemap_record(0, size);
}

static void cb_filemap_map_pages(void *unused, struct file *file, pgoff_t first,
		pgoff_t last, vm_fault_t ret)
{
	filemap_record(1, (last - first + 1) * 4096ULL);
}

static void cb_android_vh_ufs_send_command(void *unused, struct ufs_hba *hba,
		struct ufshcd_lrb *lrb)
{
	io_monitor_injection(&ufs_injection);
}

static void cb_android_vh_ufs_compl_command(void *unused, struct ufs_hba *hba,
		struct ufshcd_lrb *lrb)
{
	u64 now, ns, diff, threshold;
	unsigned int rw, i;
	int bytes;
	unsigned long flags;
	if (!io_monitor_enabled || !ufs_monitor_enabled || !lrb->cmd)
		return;
	threshold = (s64)ufs_time_threshold;
	switch (lrb->cmd->cmnd[0]) {
	case 0x08: case 0x28: case 0x88: rw = 0; break;
	case 0x0a: case 0x2a: case 0x8a: rw = 1; break;
	default: return;
	}
	now = ktime_to_ns(lrb->compl_time_stamp);
	ns = now - ktime_to_ns(lrb->issue_time_stamp);
	bytes = (int)be32_to_cpu(lrb->ucd_req_ptr->sc.exp_data_transfer_len);
	spin_lock_irqsave(&ufs_info.lock, flags);
	ufs_info.rw = rw;
	ufs_info.nr_bytes = (s64)bytes;
	for (i = 0; i < 2; i++) {
		struct io_ufs_bucket *s = &ufs_info.bucket[rw][i];
		diff = now - ufs_monitor_timestamp[i];
		s->rw = rw;
		s->last_time = ns;
		s->issue = ktime_to_ns(lrb->issue_time_stamp);
		s->compl = now;
		s->last_size = (s64)bytes;
		if (diff < now) {
			s->count++;
			s->total_size += (s64)bytes;
			s->total_time += ns;
			s->avg_time = s->count ? s->total_time / s->count : 0;
			s->max_time = max(s->max_time, ns);
			s->lat[latency_range(ns)]++;
		} else {
			diff = 0;
			ufs_monitor_timestamp[i] = now;
			s->count = 1;
			s->total_size = (s64)bytes;
			s->total_time = ns;
			s->avg_time = ns;
			s->max_time = ns;
			memset(s->lat, 0, sizeof(s->lat));
			s->lat[latency_range(ns)] = 1;
		}
		if (diff >= sample_cycle_config[i].ns)
			ufs_monitor_timestamp[i] = 0;
	}
	if (ns / 1000000 > threshold) {
		io_monitor_global_log(&ufs_info, 6);
		if (io_monitor_debug_enabled)
			IO_LOG("cb_android_vh_ufs_compl_command", 0x79, "Overtime: %llu\n", ns);
	}
	spin_unlock_irqrestore(&ufs_info.lock, flags);
	if (io_monitor_debug_enabled)
		IO_LOG("cb_android_vh_ufs_compl_command", 0x85,
			"%s %d bytes cost %llu ns begin_time:%llu, current_time:%llu\n",
			rw ? "write" : "read", bytes, ns, ktime_to_ns(lrb->issue_time_stamp), now);
}

/* Callback argument types above follow current native hook/event headers. */
#define DECLARE_TP(name) extern struct tracepoint __tracepoint_##name
DECLARE_TP(block_bio_queue); DECLARE_TP(block_getrq);
DECLARE_TP(block_rq_insert); DECLARE_TP(block_rq_issue); DECLARE_TP(block_rq_complete);
DECLARE_TP(f2fs_gc_begin); DECLARE_TP(f2fs_gc_end);
DECLARE_TP(f2fs_write_checkpoint); DECLARE_TP(f2fs_sync_file_enter); DECLARE_TP(f2fs_sync_file_exit);
DECLARE_TP(android_vh_filemap_read); DECLARE_TP(android_vh_filemap_map_pages);
DECLARE_TP(android_vh_ufs_send_command); DECLARE_TP(android_vh_ufs_compl_command);
#define REGISTER_TP(name, cb) BUG_ON(tracepoint_probe_register(&__tracepoint_##name, cb, NULL))
#define UNREGISTER_TP(name, cb) tracepoint_probe_unregister(&__tracepoint_##name, cb, NULL)

static void block_register_tracepoints(void)
{
	REGISTER_TP(block_bio_queue, cb_block_bio_queue);
	REGISTER_TP(block_getrq, cb_block_getrq);
	REGISTER_TP(block_rq_insert, cb_block_rq_insert);
	REGISTER_TP(block_rq_issue, cb_block_rq_issue);
	REGISTER_TP(block_rq_complete, cb_block_rq_complete);
}
static void block_unregister_tracepoints(void)
{
	UNREGISTER_TP(block_rq_complete, cb_block_rq_complete);
	UNREGISTER_TP(block_rq_issue, cb_block_rq_issue);
	UNREGISTER_TP(block_rq_insert, cb_block_rq_insert);
	UNREGISTER_TP(block_getrq, cb_block_getrq);
	UNREGISTER_TP(block_bio_queue, cb_block_bio_queue);
}
static void f2fs_register_tracepoints(void)
{
	REGISTER_TP(f2fs_gc_begin, cb_f2fs_gc_begin);
	REGISTER_TP(f2fs_gc_end, cb_f2fs_gc_end);
	REGISTER_TP(f2fs_write_checkpoint, cb_f2fs_write_checkpoint);
	REGISTER_TP(f2fs_sync_file_enter, cb_f2fs_sync_file_enter);
	REGISTER_TP(f2fs_sync_file_exit, cb_f2fs_sync_file_exit);
	REGISTER_TP(android_vh_filemap_read, cb_filemap_read);
	REGISTER_TP(android_vh_filemap_map_pages, cb_filemap_map_pages);
}
static void f2fs_unregister_tracepoints(void)
{
	UNREGISTER_TP(android_vh_filemap_read, cb_filemap_read);
	UNREGISTER_TP(android_vh_filemap_map_pages, cb_filemap_map_pages);
	UNREGISTER_TP(f2fs_sync_file_exit, cb_f2fs_sync_file_exit);
	UNREGISTER_TP(f2fs_sync_file_enter, cb_f2fs_sync_file_enter);
	UNREGISTER_TP(f2fs_write_checkpoint, cb_f2fs_write_checkpoint);
	UNREGISTER_TP(f2fs_gc_end, cb_f2fs_gc_end);
	UNREGISTER_TP(f2fs_gc_begin, cb_f2fs_gc_begin);
}
static void ufs_register_tracepoints(void)
{
	REGISTER_TP(android_vh_ufs_send_command, cb_android_vh_ufs_send_command);
	REGISTER_TP(android_vh_ufs_compl_command, cb_android_vh_ufs_compl_command);
}
static void ufs_unregister_tracepoints(void)
{
	UNREGISTER_TP(android_vh_ufs_compl_command, cb_android_vh_ufs_compl_command);
	UNREGISTER_TP(android_vh_ufs_send_command, cb_android_vh_ufs_send_command);
}

static void block_monitor_init(void)
{
	block_monitor_reset();
	IO_LOG("block_monitor_init", 0x1f0, "%s ok\n", "block_monitor_init");
}

static void block_monitor_exit(void)
{
	block_monitor_enabled = false;
	IO_LOG("block_monitor_exit", 0x1f7, "%s\n", "block_monitor_exit");
}

static int f2fs_monitor_init(void)
{
	f2fs_monitor_reset();
	IO_LOG("f2fs_monitor_init", 0x2f9, "%s ok\n", "f2fs_monitor_init");
	return 0;
}

static void f2fs_monitor_exit(void)
{
	f2fs_monitor_enabled = false;
	f2fs_monitor_mm_exit();
	IO_LOG("f2fs_monitor_exit", 0x303, "%s\n", "f2fs_monitor_exit");
}

static void ufs_monitor_init(void)
{
	ufs_monitor_reset();
	IO_LOG("ufs_monitor_init", 0x103, "%s ok\n", "ufs_monitor_init");
}

static void ufs_monitor_exit(void)
{
	ufs_monitor_enabled = false;
	IO_LOG("ufs_monitor_exit", 0x10a, "%s\n", "ufs_monitor_exit");
}

/* Stock .init.text+4..0x18c and .exit.text+4..0x6c, including unwind order. */
static int __init io_monitor_init(void)
{
	if (iologbuf_init() < 0) {
		IO_LOG("io_monitor_init", 0x62, "iologbuf_init failed\n");
		return -1;
	}
	if (iostat_init() < 0) {
		IO_LOG("io_monitor_init", 0x66, "iostat_init failed\n");
		goto out_misc;
	}
	if (f2fs_monitor_init() < 0) {
		IO_LOG("io_monitor_init", 0x6b, "f2fs_monitor_init failed\n");
		goto out_iostat;
	}
	if (io_monitor_enabled) {
		if (iologbuf_mm_init() < 0)
			goto out_f2fs;
		if (f2fs_monitor_mm_init() < 0)
			goto out_mm;
	}
	block_monitor_init();
	ufs_monitor_init();
	f2fs_register_tracepoints();
	block_register_tracepoints();
	ufs_register_tracepoints();
	if (!io_monitor_procfs_init()) {
		IO_LOG("io_monitor_init", 0x7f, "total alloc mem: %dK\n", total_alloc_mem / 1024);
		IO_LOG("io_monitor_init", 0x80, "%s ok\n", "io_monitor_init");
		return 0;
	}
	IO_LOG("io_monitor_init", 0x7b, "io_monitor_procfs_init failed\n");
	ufs_unregister_tracepoints();
	block_unregister_tracepoints();
	f2fs_unregister_tracepoints();
	ufs_monitor_exit();
	block_monitor_exit();
out_mm:
	iologbuf_mm_exit();
out_f2fs:
	f2fs_monitor_exit();
out_iostat:
	iostat_exit();
out_misc:
	iologbuf_exit();
	return -1;
}

static void __exit io_monitor_exit(void)
{
	io_monitor_enabled = false;
	ufs_unregister_tracepoints();
	block_unregister_tracepoints();
	f2fs_unregister_tracepoints();
	ufs_monitor_exit();
	block_monitor_exit();
	f2fs_monitor_exit();
	io_monitor_procfs_exit();
	iostat_exit();
	iologbuf_exit();
	IO_LOG("io_monitor_exit", 0xa3, "%s\n", "io_monitor_exit");
}
module_init(io_monitor_init);
module_exit(io_monitor_exit);
MODULE_DESCRIPTION("io monitor");
MODULE_VERSION("v1.0");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(ANDROID_GKI_VFS_EXPORT_ONLY);
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
