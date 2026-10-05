/* SPDX-License-Identifier: GPL-2.0 */
#ifndef XIAOMI_IO_MONITOR_H
#define XIAOMI_IO_MONITOR_H

#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

#define IO_PID_MAX 32768
#define IO_INFO_MAX 4853
#define IO_LOG_MAX 855
#define IO_READ_SIZE 0x400000
#define IO_RING_SIZE 0x100000

struct io_cycle { unsigned int id; const char *name; u64 ns; };
struct io_common {
	u64 timestamp, utc_seconds;
	char comm[16], pcomm[16], group_leader[16];
	unsigned int uid, pid, ppid, gid;
	char task_state;
	char padding[7];
};
struct io_event {
	struct io_common common;
	u8 action, rw;
	u16 padding;
	unsigned int nr_bytes;
	u64 sector, reserved;
	u64 begin[6], delay[6], tail;
};
struct io_elapsed { u64 count, total, avg, max, last, begin; };
struct io_fsync {
	spinlock_t lock;
	u32 padding;
	struct io_elapsed cycle[2];
};
struct io_gc_elapsed { u64 count, segments, total, avg, max, last, begin; };
struct io_gc {
	u32 type;
	spinlock_t lock;
	struct io_gc_elapsed cycle[2][2];
};
struct io_size_stat { u64 count, total, avg, max, last; };
struct io_filemap {
	u8 valid;
	u8 padding[3];
	u32 task_type;
	struct io_common common;
	struct io_size_stat stats[2][2][2]; /* task, read/map, cycle */
};
struct io_filemap_ctrl { spinlock_t read_lock, map_lock; struct io_filemap *info; };
struct io_block_bucket {
	u64 timestamp;
	u32 rw, padding;
	u64 count, total_size, last_size;
	u64 total[5], avg[5], max[5], last[5];
	u64 reserved[4];
	u64 lat[5][8];
};
struct io_block {
	struct io_common common;
	u32 rw, range;
	u64 reserved[4], nr_bytes;
	spinlock_t lock;
	u32 padding;
	struct io_block_bucket bucket[2][2][5]; /* rw, cycle, size */
};
struct io_ufs_bucket {
	u8 rw;
	u8 padding[7];
	u64 count, total_size, total_time, avg_time, max_time;
	u64 last_time, issue, compl, last_size, lat[8];
};
struct io_ufs {
	u32 rw, padding;
	u64 nr_bytes;
	spinlock_t lock;
	u32 padding2;
	struct io_ufs_bucket bucket[2][2];
};
struct io_log {
	u32 type, padding;
	union {
		struct { struct io_common common; u64 last; struct io_elapsed cycle[2]; } elapsed;
		struct { struct io_common common; u64 reserved, last; struct io_gc_elapsed cycle[2]; } gc;
		struct { u32 task_type, padding; u64 last; struct io_common common; struct io_size_stat cycle[2]; } filemap;
		struct { struct io_common common; u32 rw, range; u64 last, size; struct io_block_bucket cycle[2]; } block;
		struct { struct io_common common; u32 rw, padding; u64 last, size; struct io_ufs_bucket cycle[2]; } ufs;
	} data;
};
struct io_ring_info { u32 index, count; struct io_event *buf; };
struct io_ring_log { u32 index, count; u64 reserved; struct io_log *buf; };
struct io_pio {
	u8 valid;
	u8 padding[3];
	u32 pid;
	char comm[16];
	u64 rchar, wchar, syscr, syscw, read_bytes, write_bytes, cwrite_bytes;
};
struct io_iostat { struct io_pio *top, *info; struct mutex lock; };

extern bool io_monitor_enabled, io_monitor_debug_enabled, proc_show_enabled;
extern bool io_monitor_injection_enabled, iologbuf_trigger;
extern bool block_monitor_enabled, f2fs_monitor_enabled, ufs_monitor_enabled, iostat_enabled;
extern int block_time_threshold, f2fs_time_threshold[3], ufs_time_threshold;
extern int blk_injection[4], f2fs_injection[3], ufs_injection;
extern unsigned int f2fs_filemap_read_notify, f2fs_filemap_map_pages_notify;
extern struct io_cycle sample_cycle_config[2];
extern struct io_fsync f2fs_fsync_info, f2fs_cp_info;
extern struct io_gc f2fs_gc_info;
extern struct io_filemap_ctrl f2fs_read_info;
extern struct io_block block_info;
extern struct io_ufs ufs_info;
extern struct io_ring_info __io_monitor_info_ctrl;
extern struct io_ring_log __io_monitor_log_ctrl;
extern struct io_iostat iostat_info;
extern struct fasync_struct *io_monitor_async;

#define IO_LOG(fn, line, fmt, ...) pr_info("[IO_MONITOR] [%-16s] %20s:%-4d " fmt, current->comm, fn, line, ##__VA_ARGS__)
#define IO_PRINT(fmt, ...) IO_LOG(__func__, __LINE__, fmt, ##__VA_ARGS__)

void get_common_info(struct io_common *common);
void cal_total_mem(int bytes);
long io_ms_to_ns(long ms);
unsigned long io_ns_to_ms(unsigned long ns);
void io_monitor_injection(int *delay);
void io_monitor_global_log(void *info, int type);
void f2fs_monitor_reset(void);
void block_monitor_reset(void);
void ufs_monitor_reset(void);
int f2fs_monitor_mm_init(void);
void f2fs_monitor_mm_exit(void);
int iologbuf_mm_init(void);
void iologbuf_mm_exit(void);
int io_monitor_procfs_init(void);
void io_monitor_procfs_exit(void);
int iologbuf_init(void);
void iologbuf_exit(void);
int iostat_init(void);
void iostat_exit(void);
unsigned int f2fs_read_info_output(struct seq_file *m, char *buf, unsigned int off);
unsigned int iostat_output(struct seq_file *m, char *buf, unsigned int off);

#endif
