// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: seven stock scheduling-latency bodies. No hooks/state integration. */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include "metis_common.h"
#include "metis_schlat.h"

static __always_inline void schlat_stock_index_failure(void)
{
	asm volatile("brk #0x5512");
	__builtin_unreachable();
}

static __always_inline void schlat_stock_object_bound(bool outside)
{
	if (outside) {
		asm volatile("brk #1");
		__builtin_unreachable();
	}
}

void metis_schlat_schedule_hook(struct task_struct *prev,
			       struct task_struct *next, unsigned int cpu)
{
	struct metis_schlat_ring *ring;
	struct metis_schlat_switch *entry;
	u64 cpu_offset, entry_offset, timestamp_offset;
	u32 index, capture_index, count;

	if (!metis_schlat_enable || (metis_schctx_enable & 1) || !prev || !next)
		return;
	if (cpu >= 32)
		schlat_stock_index_failure();
	cpu_offset = (u64)cpu * 0x470;
	schlat_stock_object_bound((cpu_offset | 4) > 0x8e00);
	ring = &cpu_sched[cpu];
	index = READ_ONCE(ring->next_index);
	if (index >= 10)
		schlat_stock_index_failure();
	entry_offset = cpu_offset + (u64)index * 0x70 + 0x10;
	entry = &ring->entries[index];
	/* Retain the original driver-object bounds and their write ordering. */
	schlat_stock_object_bound(entry_offset > 0x8dec);
	WRITE_ONCE(entry->prev.pid, prev->pid);
	schlat_stock_object_bound(entry_offset > 0x8dbc);
	WRITE_ONCE(entry->next.pid, next->pid);
	schlat_stock_object_bound(entry_offset > 0x8dd8);
	WRITE_ONCE(entry->prev.prio, prev->prio);
	schlat_stock_object_bound(entry_offset > 0x8da8);
	WRITE_ONCE(entry->next.prio, next->prio);
	memcpy(entry->prev.comm, prev->comm, 16);
	memcpy(entry->next.comm, next->comm, 16);
	/* Stock reads the clock three times, rather than sharing one snapshot. */
	{
		s64 now = ktime_to_ns(ktime_get()) / 1000;

		schlat_stock_object_bound((entry_offset >> 9) >= 0x47);
		WRITE_ONCE(entry->switch_us, (u64)now);
	}
	{
		s64 now = ktime_to_ns(ktime_get()) / 1000;

		timestamp_offset = cpu_offset | 8;
		schlat_stock_object_bound(timestamp_offset > 0x8e00 ||
					 entry_offset + 8 > 0x8e00);
		WRITE_ONCE(entry->interval_us,
			   (u64)now - READ_ONCE(ring->last_switch_us));
	}
	{
		s64 now = ktime_to_ns(ktime_get()) / 1000;

		schlat_stock_object_bound(timestamp_offset > 0x8e00);
		WRITE_ONCE(ring->last_switch_us, (u64)now);
	}
	capture_index = READ_ONCE(long_schlat.context_index);
	count = READ_ONCE(long_schlat.count);
	schlat_stock_object_bound((cpu_offset | 4) > 0x8e00);
	WRITE_ONCE(ring->next_index, index < 9 ? index + 1 : 0);
	if (capture_index < count) {
		if (capture_index >= 100)
			schlat_stock_index_failure();
		if (READ_ONCE(long_schlat.contexts[capture_index].cpu) != cpu)
			return;
		/* Whole-ring copy also overwrites the recorded CPU with ring->cpu. */
		memcpy(&long_schlat.contexts[capture_index], ring, 0x470);
		WRITE_ONCE(long_schlat.context_index, capture_index + 1);
	} else if (capture_index == 100) {
		WRITE_ONCE(metis_schctx_enable, 1);
	}
}

void metis_schlat_dequeue_entity_hook(unsigned int cpu, struct task_struct *task)
{
	struct mi_task_struct *state;
	struct metis_schlat_record *record;
	u64 enqueue, latency_ns, latency_us;
	u32 milliseconds, count, total_us;

	if (!metis_schlat_enable)
		return;
	state = get_mi_task_struct(task);
	if (!state)
		return;
	latency_ns = fas_sched_clock();
	enqueue = MI_FIELD(state, u64, 0x280);
	latency_ns -= enqueue;
	milliseconds = (u32)(latency_ns / 1000000);
	(void)atomic64_fetch_add_relaxed(1,
		&schlat_region_cnt[milliseconds < 10 ? milliseconds : 10]);
	latency_us = latency_ns / 1000;
	(void)atomic64_fetch_add_relaxed(1, &schlat_acc.count);
	(void)atomic64_fetch_add_relaxed((s64)latency_us, &schlat_acc.sum_us);
	if (milliseconds >= 10) {
		count = READ_ONCE(long_schlat.count);
		if (count < 100) {
			record = &long_schlat.samples[count];
			WRITE_ONCE(record->tgid, task->tgid);
			WRITE_ONCE(record->pid, task->pid);
			total_us = READ_ONCE(long_schlat.total_us);
			WRITE_ONCE(record->latency_us, latency_us);
			WRITE_ONCE(record->enqueue_us, enqueue / 1000);
			/* Stock publishes its count/sum before the CPU and comm copy. */
			WRITE_ONCE(long_schlat.count, count + 1);
			WRITE_ONCE(long_schlat.total_us, total_us + (u32)latency_us);
			WRITE_ONCE(long_schlat.contexts[count].cpu, cpu);
			memcpy(record->comm, task->comm, 16);
		}
	}
	if (READ_ONCE(long_schlat.maximum.latency_us) < latency_us) {
		WRITE_ONCE(long_schlat.maximum.latency_us, latency_us);
		WRITE_ONCE(long_schlat.maximum.tgid, task->tgid);
		WRITE_ONCE(long_schlat.maximum.pid, task->pid);
		memcpy(long_schlat.maximum.comm, task->comm, 16);
	}
}

int metis_schlat_clear(const char *value, const struct kernel_param *param)
{
	int parsed = 0;
	unsigned int i;

	if (sscanf(value, "%d", &parsed) != 1)
		return -EINVAL;
	if (parsed != 1)
		return 0;
	for (i = 0; i < 11; i++)
		atomic64_set(&schlat_region_cnt[i], 0);
	atomic64_set(&schlat_acc.sum_us, 0);
	atomic64_set(&schlat_acc.count, 0);
	WRITE_ONCE(long_schlat.count_total, 0);
	WRITE_ONCE(long_schlat.context_index, 0);
	WRITE_ONCE(long_schlat.maximum.latency_us, 0);
	WRITE_ONCE(metis_schctx_enable, 0);
	return 0;
}

int metis_schlat_show(struct seq_file *seq, void *unused)
{
	u64 sum = (u64)atomic64_read(&schlat_acc.sum_us);
	u64 count = (u64)atomic64_read(&schlat_acc.count);
	u32 divisor = (u32)count;
	u32 long_count, total_us;
	u64 sample_index;
	unsigned int region;

	seq_printf(seq, "---------------metis sched latency stats------------------\n");
	for (region = 0; region < 10; region++)
		seq_printf(seq, "region %2d: [ %5d, %5d ) us, schlat_cnt: %10lld\n",
			   (int)region, (int)region * 1000, (int)(region + 1) * 1000,
			   (s64)atomic64_read(&schlat_region_cnt[region]));
	seq_printf(seq, "region %2d: [ %5d, %5s ) us, schlat_cnt: %10lld\n",
		   10, 10000, "inf", (s64)atomic64_read(&schlat_region_cnt[10]));
	seq_printf(seq, "\naverage schduling latency [> 0 ms]:\n");
	/* AArch64 UDIV returns zero for a zero divisor; C must express it. */
	seq_printf(seq, "\tavg_sched_latency: ( %lld / %lld ) = %lld\n",
		   (s64)sum, (s64)count, (s64)(divisor ? sum / divisor : 0));
	seq_printf(seq, "\nmax schduling latency msg:\n");
	seq_printf(seq, "\tmax_sched_latency: %llu, pid = %d, name = %s\n",
		   READ_ONCE(long_schlat.maximum.latency_us),
		   READ_ONCE(long_schlat.maximum.pid), long_schlat.maximum.comm);
	long_count = READ_ONCE(long_schlat.count);
	total_us = READ_ONCE(long_schlat.total_us);
	seq_printf(seq, "\tavg_long_sched_latency [> 10ms]: ( %u / %u ) = %ld \n",
		   total_us, long_count,
		   (long)(long_count ? total_us / long_count : 0));
	if (!READ_ONCE(long_schlat.count))
		return 0;
	sample_index = 0;
	do {
		struct metis_schlat_record *record;
		struct metis_schlat_ring *ring;
		u64 ring_offset, base;
		u32 cursor;
		unsigned int remaining;

		seq_printf(seq, "\n------------\t long task No.%d\t------------\n",
			   (s32)(sample_index + 1));
		if (sample_index == 100)
			schlat_stock_index_failure();
		record = &long_schlat.samples[sample_index];
		seq_printf(seq, " schlat_time = %8llu, enqueue_time = %llu, pid = %8d, name = %18s\n",
			   READ_ONCE(record->latency_us), READ_ONCE(record->enqueue_us),
			   READ_ONCE(record->pid), record->comm);
		ring_offset = sample_index * 0x470;
		schlat_stock_object_bound(ring_offset - 0x1bbbd < 0xfffffffffffe313fULL);
		base = ring_offset + 0x1310;
		ring = &long_schlat.contexts[sample_index];
		cursor = READ_ONCE(ring->next_index);
		for (remaining = 10; remaining; remaining--) {
			struct metis_schlat_switch *entry;
			u32 index = (s32)cursor > 9 ? cursor - 10 : cursor;
			u64 offset, pos;

			if (index > 9) {
				seq_printf(seq, "\t task sched switch context error!!!\n");
				break;
			}
			offset = base + (u64)index * 0x70;
			entry = &ring->entries[index];
			if ((unsigned long)entry) {
				schlat_stock_object_bound((offset >> 6) > 0x73a ||
							 (offset | 8) > 0x1cec0);
				pos = ring_offset - 0x1bbb1 + (u64)index * 0x70;
				schlat_stock_object_bound(pos + 0x14 < 0xfffffffffffe313fULL ||
					pos + 0x28 < 0xfffffffffffe313fULL ||
					offset - 0x1ce7d < 0xfffffffffffe313fULL ||
					offset - 0x1ce69 < 0xfffffffffffe313fULL);
				seq_printf(seq, "\tswitch_tm = %llu intv_tm = %llu\tprev_pid = %-6d  prio = %-4d  prev_name = %-16s\tnext_pid = %-6d  prio = %-4d  next_name = %-16s\n",
					READ_ONCE(entry->switch_us), READ_ONCE(entry->interval_us),
					READ_ONCE(entry->prev.pid), READ_ONCE(entry->prev.prio),
					entry->prev.comm, READ_ONCE(entry->next.pid),
					READ_ONCE(entry->next.prio), entry->next.comm);
			}
			cursor = index + 1;
		}
		sample_index++;
	} while (sample_index < READ_ONCE(long_schlat.count));
	return 0;
}

int metis_schlat_open(struct inode *inode, struct file *file)
{
	return single_open(file, metis_schlat_show, inode->i_private);
}

/* .rodata+0xc78: zero raw bytes plus these four ABS64 relocations. */
static const struct proc_ops metis_schlat_proc_ops = {
	.proc_open = metis_schlat_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

int metis_schlat_init(void)
{
	schlat_entry = proc_create("show_metis_schlat", 0644, NULL,
				   &metis_schlat_proc_ops);
	pr_info("schlat_stats: metis sched latency init!\n");
	return 0;
}

void metis_schlat_exit(void)
{
	if (schlat_entry)
		proc_remove(schlat_entry);
	pr_info("schlat_stats: metis sched latency exit!\n");
	/* The original leaves schlat_entry unchanged. */
}
