/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_LB_H
#define _XIAOMI_LB_H

#include <linux/types.h>
#include <linux/seq_file.h>
#include <linux/printk.h>

/* Stock .text mi_lb_ioctl+0x0..0x884: all command numbers encode 264. */
struct lb_command {
	u32 uid;
	s32 pid;
	char name[256];
};
struct lb_size_command {
	struct lb_command app;
	u64 size;
};
struct lb_info_command {
	struct lb_command app;
	u32 file_count;
	u32 reserved;
	u64 size;
	u8 files[];
};
struct lb_wire_file {
	s64 mtime_sec;
	s64 mtime_nsec;
	u64 ino;
	u64 pages;
	char path[256];
	u32 bitmap_words;
	u32 reserved;
	u64 bitmap[];
};
struct lb_app_names {
	u32 count;
	struct lb_command apps[100];
};

extern int g_lb_msg_level;
extern bool g_atrace_enable, g_debug_verbose;
extern bool g_pte_anon_swapin_enable, g_pte_fadvise_enable;
extern bool g_pte_mark_file_anon_enable;
extern bool g_swapin_expire_interruput_enable, g_rlock_expire_interruput_enable;
extern unsigned int g_max_pte_segment_count, g_pte_max_active_apps;
extern unsigned long long g_swapin_total_time_threshold, g_rlock_acquire_threshold;

/* All stock diagnostics carry KERN_NOTICE, including the E/D/I labels. */
#define LB_MSG(level, kind, function, line, fmt, ...) do { \
	if (g_lb_msg_level >= (level)) \
		printk(KERN_NOTICE "[LB " kind "]:%s %d: " fmt, \
		       function, line, ##__VA_ARGS__); \
} while (0)
#define LB_E(function, line, fmt, ...) LB_MSG(0, "E", function, line, fmt, ##__VA_ARGS__)
#define LB_D(function, line, fmt, ...) LB_MSG(3, "D", function, line, fmt, ##__VA_ARGS__)
#define LB_I(function, line, fmt, ...) LB_MSG(2, "I", function, line, fmt, ##__VA_ARGS__)

int lb_hot_init(void);
void lb_hot_exit(void);
int lb_hot_collect_show(struct seq_file *s, void *unused);
int mi_lb_dealwith_pte_snapshot_cmd(struct lb_command *cmd);
int mi_lb_dealwith_pte_preread_cmd(struct lb_command *cmd);
int mi_lb_dealwith_pte_interrupt_cmd(struct lb_command *cmd);
int mi_lb_dealwith_pte_clean_cmd(struct lb_command *cmd);

#endif
