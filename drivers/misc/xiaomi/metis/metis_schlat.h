/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_SCHLAT_H
#define _XIAOMI_METIS_SCHLAT_H
#include <linux/atomic.h>
#include <linux/types.h>
struct task_struct;
struct inode;
struct file;
struct seq_file;
struct kernel_param;
struct proc_dir_entry;

/* Driver-owned layouts recovered from stores, strides and symbol sizes. */
struct metis_schlat_record {
	s32 tgid;                         /* +0x00 */
	s32 pid;                          /* +0x04 */
	char comm[16];                    /* +0x08 */
	u8 unknown_18[8];
	u64 latency_us;                   /* +0x20 */
	u64 enqueue_us;                   /* +0x28; total 0x30 */
};

struct metis_schlat_task_context {
	s32 unknown_00;                   /* Schedule producer does not write it. */
	s32 pid;                          /* +0x04 */
	char comm[16];                    /* +0x08 */
	s32 prio;                         /* +0x18 */
	u8 unknown_1c[0x14];              /* total 0x30 */
};

struct metis_schlat_switch {
	u64 switch_us;                    /* +0x00 */
	u64 interval_us;                  /* +0x08 */
	struct metis_schlat_task_context prev; /* +0x10 */
	struct metis_schlat_task_context next; /* +0x40; total 0x70 */
};

struct metis_schlat_ring {
	u32 cpu;                          /* +0x00; owner producer unknown */
	u32 next_index;                   /* +0x04 */
	u64 last_switch_us;               /* +0x08 */
	struct metis_schlat_switch entries[10]; /* +0x10; total 0x470 */
};

struct metis_schlat_long_state {
	union {
		struct {
			u32 count;                /* +0x0000 */
			u32 total_us;             /* +0x0004; wraps at 32 bits */
		};
		u64 count_total;                  /* Clear uses one 64-bit store. */
	};
	u32 context_index;                /* +0x0008 */
	u32 unknown_0c;
	struct metis_schlat_record maximum;     /* +0x0010 */
	struct metis_schlat_record samples[100]; /* +0x0040 */
	struct metis_schlat_ring contexts[100]; /* +0x1300; total 0x1cec0 */
};

struct metis_schlat_accumulator {
	atomic64_t sum_us;                /* +0x00 */
	atomic64_t count;                 /* +0x08 */
};

/* Actual stock owners remain extern; no substitute BSS/defaults are added. */
extern u32 metis_schlat_enable;     /* .bss+0x160d8 */
extern u8 metis_schctx_enable;      /* .bss+0x160dc */
extern struct proc_dir_entry *schlat_entry; /* .bss+0x160e0 */
extern struct metis_schlat_ring cpu_sched[32]; /* .bss+0x160e8, size 0x8e00 */
extern struct metis_schlat_long_state long_schlat; /* .bss+0x1eee8 */
extern atomic64_t schlat_region_cnt[11]; /* .bss+0x3bda8 */
extern struct metis_schlat_accumulator schlat_acc; /* .bss+0x3be00 */

void metis_schlat_schedule_hook(struct task_struct *prev,
			       struct task_struct *next, unsigned int cpu);
void metis_schlat_dequeue_entity_hook(unsigned int cpu, struct task_struct *task);
int metis_schlat_clear(const char *value, const struct kernel_param *param);
int metis_schlat_show(struct seq_file *seq, void *unused);
int metis_schlat_open(struct inode *inode, struct file *file);
int metis_schlat_init(void);
void metis_schlat_exit(void);
#endif
