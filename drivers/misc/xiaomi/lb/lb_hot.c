// SPDX-License-Identifier: GPL-2.0-only
/* Stock lb .text hot path 0x3140..0x4fc8; native kernel fields replace offsets. */
#include <linux/bitmap.h>
#include <linux/cred.h>
#include <uapi/linux/fadvise.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/module.h>
#include <linux/pagewalk.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/xarray.h>
#include <trace/hooks/sched.h>
#include "lb.h"

/* Exported by mm/swap_state.c; its declaration lives in the core's mm/swap.h. */
struct swap_iocb;
extern struct page *read_swap_cache_async(swp_entry_t entry, gfp_t gfp,
		struct vm_area_struct *vma, unsigned long addr, struct swap_iocb **plug);

struct lb_hot_manager;
struct lb_pte_record {
	pid_t pid;
	u32 uid;
	struct mm_struct *mm;
	struct xarray segments[2];
	struct rcu_head rcu;
	struct mutex rec_lock;
	unsigned long *last_segment;
	unsigned long last_index;
	u32 last_type;
	atomic_t refs;
	struct lb_hot_manager *manager;
	struct list_head list;
	unsigned long created;
	u32 segment_count, anon_pages, file_pages;
	u32 active_pages, young_referenced_pages, referenced_pages, young_pages;
	u32 pre_anon, pre_file;
	u32 unused[3];
	u32 hit_anon, hit_file;
	u32 interrupt;
	s64 rlock_start, rlock_time, swapin_start, swapin_time;
	s64 fadvise_start, fadvise_time, preread_start, preread_time;
};
struct lb_hot_manager {
	spinlock_t lock;
	unsigned long unused;
	struct kmem_cache *segment_cache;
	struct list_head records;
	struct xarray by_mm;
	atomic_t segment_count, record_count;
	u32 active_count, preread_count, unused_count, errors;
	u64 anon_pages, pre_anon, file_pages, pre_file;
	/* Stock path starts at 0x70; its 256-byte window overlaps max_time
	 * and the first 120 bytes of max_name, starting at 0xf0. */
	union {
		char path[256];
		struct {
			char scratch[128];
			s64 max_time;
			char max_name[256];
			s64 min_time;
			char min_name[256];
		};
	};
	atomic64_t rlock_time, swapin_time, fadvise_time, total_time, total_count;
	atomic64_t interrupt_ioctl, interrupt_rlock, interrupt_swapin;
};
static struct lb_hot_manager g_hot_mps;

static s64 lb_time_us(void)
{
	return ktime_to_ns(ktime_get()) / 1000;
}
/* mps_alloc_seg 0x4af8..0x4b70: limit and increment are separate. */
static unsigned long *mps_alloc_seg(struct lb_hot_manager *m)
{
	unsigned long *segment = NULL;

	if ((u32)atomic_read(&m->segment_count) < g_max_pte_segment_count) {
		segment = kmem_cache_alloc(m->segment_cache, GFP_NOWAIT | __GFP_ZERO);
		if (segment)
			atomic_inc(&m->segment_count);
	}
	return segment;
}
static void mps_free_seg(struct lb_hot_manager *m, unsigned long *segment)
{
	atomic_dec(&m->segment_count);
	kmem_cache_free(m->segment_cache, segment);
}
/* mps_delayed_free_rec 0x4bc8..0x4db8 */
static void mps_delayed_free_rec(struct rcu_head *rcu)
{
	struct lb_pte_record *r = container_of(rcu, struct lb_pte_record, rcu);
	struct lb_hot_manager *m = r->manager;
	unsigned long index, *segment;
	unsigned int type;

	mmdrop(r->mm);
	for (type = 0; type < 2; type++) {
		index = 0;
		segment = xa_find(&r->segments[type], &index, ULONG_MAX, XA_PRESENT);
		while (segment) {
			mps_free_seg(m, segment);
			xa_erase(&r->segments[type], index);
			segment = xa_find_after(&r->segments[type], &index, ULONG_MAX, XA_PRESENT);
		}
	}
	kfree(r);
	atomic_dec(&m->record_count);
}
static void mps_put_record(struct lb_pte_record *r)
{
	if (atomic_dec_and_test(&r->refs))
		call_rcu(&r->rcu, mps_delayed_free_rec);
}
/* mps_unlink_rec_locked 0x446c..0x4598 only removes at refs == 1. */
static void mps_unlink_rec_locked(struct mm_struct *mm)
{
	struct lb_pte_record *r = xa_load(&g_hot_mps.by_mm, (unsigned long)mm);

	if (!r || r->mm != mm || atomic_read(&r->refs) != 1)
		return;
	g_hot_mps.pre_anon += r->pre_anon;
	g_hot_mps.pre_file += r->pre_file;
	g_hot_mps.active_count--;
	list_del(&r->list);
	xa_erase(&g_hot_mps.by_mm, (unsigned long)mm);
	mps_put_record(r);
}
/* mps_find_get_record 0x33e0..0x392c: a second snapshot drops the
 * existing lookup reference and returns NULL instead of refreshing it. */
static struct lb_pte_record *mps_find_get_record(pid_t pid, bool create)
{
	struct task_struct *task;
	struct mm_struct *mm;
	struct lb_pte_record *r;
	pid_t tgid;
	u32 uid;
	int ret;

	task = get_pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) {
		LB_E("mps_find_get_record", 0x13a, "get_pid_task %u failed", pid);
		return NULL;
	}
	mm = get_task_mm(task);
	if (!mm) {
		put_task_struct(task);
		return NULL;
	}
	tgid = task->tgid;
	rcu_read_lock();
	uid = __kuid_val(task_uid(task));
	rcu_read_unlock();
	spin_lock(&g_hot_mps.lock);
	r = xa_load(&g_hot_mps.by_mm, (unsigned long)mm);
	if (r && (r->mm != mm || !atomic_inc_not_zero(&r->refs)))
		r = NULL;
	spin_unlock(&g_hot_mps.lock);
	if (!create) {
		if (r && (r->pid != tgid || r->uid != uid)) {
			LB_E("mps_find_check_record", 0x127, "mps_rec %u %d not equal task %u %u",
			     r->pid, r->uid, tgid, uid);
			mps_put_record(r);
			r = NULL;
		}
		goto out;
	}
	if (r) {
		mps_put_record(r);
		r = NULL;
		goto out;
	}
	r = kzalloc(sizeof(*r), GFP_NOWAIT);
	if (!r) {
		LB_E("mps_alloc_record", 0xf3, "alloc mps_rec %u failed", tgid);
		goto out;
	}
	atomic_inc(&g_hot_mps.record_count);
	r->pid = tgid;
	r->uid = uid;
	xa_init(&r->segments[0]);
	xa_init(&r->segments[1]);
	mutex_init(&r->rec_lock);
	atomic_set(&r->refs, 1);
	r->manager = &g_hot_mps;
	r->mm = mm;
	r->created = jiffies;
	spin_lock(&g_hot_mps.lock);
	if (g_hot_mps.active_count > g_pte_max_active_apps - 1) {
		struct lb_pte_record *old = list_last_entry(&g_hot_mps.records, struct lb_pte_record, list);

		LB_D("mps_link_rec", 0xda, "shrink app, uid: %u, pid: %u", old->uid, old->pid);
		mps_unlink_rec_locked(old->mm);
	}
	ret = xa_insert(&g_hot_mps.by_mm, (unsigned long)mm, r, GFP_NOWAIT);
	if (!ret) {
		atomic_inc(&r->refs);
		list_add(&r->list, &g_hot_mps.records);
		g_hot_mps.active_count++;
		spin_unlock(&g_hot_mps.lock);
		mmgrab(mm);
		goto out;
	}
	LB_E("mps_link_rec", 0xdf, "insert mps_rec encounter: %d", ret);
	spin_unlock(&g_hot_mps.lock);
	LB_E("mps_alloc_record", 0x103, "mps_rec %u can not link", tgid);
	kfree(r);
	atomic_dec(&g_hot_mps.record_count);
	r = NULL;
 out:
	mmput(mm);
	put_task_struct(task);
	return r;
}
/* mps_rec_load_seg 0x49fc..0x4af4 */
static unsigned long *mps_rec_load_seg(struct lb_pte_record *r, unsigned long index, u32 type)
{
	struct xarray *array = &r->segments[type];
	unsigned long *segment = xa_load(array, index);

	if (!segment) {
		segment = mps_alloc_seg(r->manager);
		if (!segment) {
			LB_E("mps_rec_load_seg", 0x4d, "alloc seg failed");
			return NULL;
		}
		if (xa_insert(array, index, segment, GFP_NOWAIT)) {
			mps_free_seg(r->manager, segment);
			LB_E("mps_rec_load_seg", 0x53, "insert seg failed");
			return NULL;
		}
		r->segment_count++;
	}
	r->last_segment = segment;
	r->last_index = index;
	r->last_type = type;
	return segment;
}
/* mps_hot_pte_range 0x4790..0x49fc */
static int mps_hot_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end, struct mm_walk *walk)
{
	struct lb_pte_record *r = walk->private;
	struct vm_area_struct *vma = walk->vma;
	spinlock_t *ptl;
	pte_t *pte, *base;
	unsigned int type;
	u32 count = 0;
	int ret = 0;

	if (pmd_trans_huge(READ_ONCE(*pmd)) || pmd_devmap(READ_ONCE(*pmd)))
		return 0;
	if ((!g_pte_anon_swapin_enable && !vma->vm_file) ||
	    (!g_pte_fadvise_enable && vma->vm_file))
		return 0;
	type = g_pte_mark_file_anon_enable && vma->vm_file;
	base = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	if (!base)
		return 0;
	pte = base;
	for (; addr != end; addr += 4096, pte++) {
		pte_t entry = ptep_get(pte);
		struct page *page;
		unsigned long *segment;
		unsigned long index;

		if (!pte_present(entry))
			continue;
		page = vm_normal_page(vma, addr, entry);
		if (!page)
			continue;
		if (PageActive(page)) {
			r->active_pages++;
		} else {
			if (pte_young(entry)) {
				r->young_pages++;
				if (!PageReferenced(page))
					continue;
				r->young_referenced_pages++;
				r->referenced_pages++;
			} else {
				if (PageReferenced(page))
					r->referenced_pages++;
				continue;
			}
		}
		index = addr >> 21;
		segment = r->last_segment;
		if (r->last_index != index || r->last_type != type || !segment)
			segment = mps_rec_load_seg(r, index, type);
		if (!segment) {
			ret = -ENOMEM;
			break;
		}
		count++;
		__set_bit((addr >> 12) & 511, segment);
	}
	pte_unmap_unlock(base, ptl);
	if (!vma->vm_file)
		r->anon_pages += count;
	else
		r->file_pages += count;
	return ret;
}
int mi_lb_dealwith_pte_snapshot_cmd(struct lb_command *cmd)
{
	static const struct mm_walk_ops ops = { .pmd_entry = mps_hot_pte_range };
	struct lb_pte_record *r;
	struct vm_area_struct *vma;
	int ret = 0;

	LB_D("mi_lb_dealwith_pte_snapshot_cmd", 0x2b0, "uid: %d, pid: %d, packagename: %s",
	     cmd->uid, cmd->pid, cmd->name);
	r = mps_find_get_record(cmd->pid, true);
	if (!r) {
		LB_E("mi_lb_dealwith_pte_snapshot_cmd", 0x2b4, "mps_rec %u maybe has snapshot", cmd->pid);
		return 0;
	}
	mutex_lock(&r->rec_lock);
	mmap_read_lock(r->mm);
	{
		VMA_ITERATOR(vmi, r->mm, 0);

		for_each_vma(vmi, vma) {
			/* The stock load at snapshot+0xf8 reads vm_end's upper word. */
			if ((!vma->vm_file && !(u32)(vma->vm_end >> 32)) ||
			    (vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP)))
				continue;
			ret = walk_page_range(r->mm, vma->vm_start, vma->vm_end, &ops, r);
			if (ret)
				break;
		}
	}
	mmap_read_unlock(r->mm);
	spin_lock(&g_hot_mps.lock);
	g_hot_mps.anon_pages += r->anon_pages;
	g_hot_mps.file_pages += r->file_pages;
	spin_unlock(&g_hot_mps.lock);
	mutex_unlock(&r->rec_lock);
	mps_put_record(r);
	if (ret)
		g_hot_mps.errors++;
	return ret;
}
struct lb_swapin_context {
	struct lb_pte_record *record;
	struct vm_area_struct *vma;
};
/* mps_swapin_walk_pmd_entry 0x4dbc..0x4f80 */
static int mps_swapin_walk_pmd_entry(pmd_t *pmd, unsigned long addr, unsigned long end, struct mm_walk *walk)
{
	struct lb_swapin_context *ctx = walk->private;
	spinlock_t *ptl = NULL;
	pte_t *pte = NULL, *base = NULL;

	for (; addr < end; addr += 4096) {
		pte_t entry;
		struct page *page;
		swp_entry_t swap;

		if (!pte) {
			base = pte_offset_map_lock(ctx->vma->vm_mm, pmd, addr, &ptl);
			pte = base;
			if (!pte)
				return 0;
		} else {
			pte++;
		}
		entry = ptep_get(pte);
		if (pte_none(entry) || pte_present(entry))
			continue;
		swap = pte_to_swp_entry(entry);
		if (non_swap_entry(swap))
			continue;
		pte_unmap_unlock(base, ptl);
		pte = NULL;
		/* Stock 0x2100cca = GFP_HIGHUSER_MOVABLE | __GFP_SKIP_KASAN. */
		page = read_swap_cache_async(swap, GFP_HIGHUSER_MOVABLE | __GFP_SKIP_KASAN,
					    ctx->vma, addr, NULL);
		if (page) {
			put_page(page);
			ctx->record->pre_anon++;
		}
	}
	if (pte)
		pte_unmap_unlock(base, ptl);
	return 0;
}
/* Inlined mps_rec_prepage_seg_range, preread+0x158..0x514. */
static int mps_prepage_range(struct lb_pte_record *r, unsigned long start, unsigned long end, u32 flags)
{
	static const struct mm_walk_ops ops = { .pmd_entry = mps_swapin_walk_pmd_entry };
	struct vm_area_struct *vma;
	struct file *file = NULL;
	struct lb_swapin_context ctx;
	u64 acquire;
	loff_t offset = 0;
	bool expired;
	int ret = 0;

	r->rlock_start = lb_time_us();
	down_read(&r->mm->mmap_lock);
	acquire = lb_time_us() - r->rlock_start;
	expired = g_rlock_expire_interruput_enable && acquire > g_rlock_acquire_threshold;
	if (expired) {
		r->interrupt = 1;
		atomic64_inc(&r->manager->interrupt_rlock);
	}
	r->rlock_time += acquire;
	vma = find_vma(r->mm, start);
	if (vma && !(vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP)) &&
	    (vma->vm_file || (flags & 0x10)) && (!vma->vm_file || (flags & 0x20))) {
		end = min(end, vma->vm_end);
		if (vma->vm_file && get_file_rcu(vma->vm_file)) {
			file = vma->vm_file;
			offset = start - vma->vm_start + (vma->vm_pgoff << 12);
		} else if (!expired) {
			r->swapin_start = lb_time_us();
			ctx.record = r;
			ctx.vma = vma;
			ret = walk_page_range(r->mm, start, min(end, vma->vm_end), &ops, &ctx);
			ret = min(ret, 0);
			r->swapin_time += lb_time_us() - r->swapin_start;
		}
	}
	up_read(&r->mm->mmap_lock);
	if (file) {
		if (g_atrace_enable) {
			char *path = file_path(file, r->manager->path, 256);

			LB_D("mps_rec_prepage_seg_range", 0x24e, "pre_hot_file:%s [0x%lld 0x%lld]",
			     path, offset, (loff_t)(offset + end - start));
		}
		r->fadvise_start = lb_time_us();
		ret = vfs_fadvise(file, offset, end - start, POSIX_FADV_WILLNEED);
		if (!ret)
			r->pre_file += (end - start) >> 12;
		fput(file);
		r->fadvise_time += lb_time_us() - r->fadvise_start;
	}
	return ret;
}
/* preread .text 0x3934..0x4250 */
int mi_lb_dealwith_pte_preread_cmd(struct lb_command *cmd)
{
	struct lb_pte_record *r;
	struct mm_struct *mm;
	u32 anon, file, flags = 0x30;
	unsigned int type;
	int ret = 0;
	s64 total;

	LB_D("mi_lb_dealwith_pte_preread_cmd", 0x2e4, "uid: %d, pid: %d, packagename: %s",
	     cmd->uid, cmd->pid, cmd->name);
	r = mps_find_get_record(cmd->pid, false);
	if (!r) {
		LB_E("mi_lb_dealwith_pte_preread_cmd", 0x2e8, "mps_rec %u maybe no snapshot", cmd->pid);
		return 0;
	}
	mutex_lock(&r->rec_lock);
	anon = r->anon_pages;
	file = r->file_pages;
	r->pre_anon = r->pre_file = 0;
	r->preread_start = lb_time_us();
	for (type = 0; type < 2; type++) {
		unsigned long index = 0, *segment;

		if (type && !(g_pte_mark_file_anon_enable && g_pte_fadvise_enable))
			break;
		segment = xa_find(&r->segments[type], &index, ULONG_MAX, XA_PRESENT);
		while (segment && !r->interrupt) {
			u32 first = find_next_bit(segment, 512, 0);
			u32 last = find_next_zero_bit(segment, 512, first + 1);

			while (first < last && !r->interrupt) {
				unsigned long start = (index << 21) + ((unsigned long)first << 12);

				ret = mps_prepage_range(r, start, start + ((unsigned long)(last - first) << 12), flags);
				if (ret)
					break;
				first = find_next_bit(segment, 512, last + 1);
				last = find_next_zero_bit(segment, 512, first + 1);
			}
			if (anon <= r->pre_anon)
				flags &= ~0x10;
			if (file <= r->pre_file)
				flags &= ~0x20;
			if (ret == -ENOMEM || !flags)
				break;
			if (g_swapin_expire_interruput_enable &&
			    g_swapin_total_time_threshold < (u64)(lb_time_us() - r->preread_start)) {
				r->interrupt = 1;
				atomic64_inc(&r->manager->interrupt_swapin);
				break;
			}
			segment = xa_find_after(&r->segments[type], &index, ULONG_MAX, XA_PRESENT);
		}
		if (segment && r->interrupt)
			break;
	}
	r->preread_time += lb_time_us() - r->preread_start;
	LB_D("mps_rec_prepage", 0x2a5,
	     "mps_rec %u ret %d prepage %u %u seg %u, rlock: %lld, swapin: %lld, fadvise: %lld, total: %lld, interrupt: %d",
	     r->pid, ret, r->pre_anon, r->pre_file, r->segment_count, r->rlock_time,
	     r->swapin_time, r->fadvise_time, r->preread_time, r->interrupt);
	spin_lock(&g_hot_mps.lock);
	g_hot_mps.preread_count++;
	spin_unlock(&g_hot_mps.lock);
	mutex_unlock(&r->rec_lock);
	total = r->preread_time;
	if (total < g_hot_mps.min_time) {
		g_hot_mps.min_time = total;
		memcpy(g_hot_mps.min_name, cmd->name, 256);
	}
	if (g_hot_mps.max_time < total) {
		g_hot_mps.max_time = total;
		memcpy(g_hot_mps.max_name, cmd->name, 256);
	}
	atomic64_add(r->rlock_time, &g_hot_mps.rlock_time);
	atomic64_add(r->swapin_time, &g_hot_mps.swapin_time);
	atomic64_add(r->fadvise_time, &g_hot_mps.fadvise_time);
	atomic64_add(r->preread_time, &g_hot_mps.total_time);
	atomic64_inc(&g_hot_mps.total_count);
	mm = r->mm;
	mps_put_record(r);
	spin_lock(&g_hot_mps.lock);
	mps_unlink_rec_locked(mm);
	spin_unlock(&g_hot_mps.lock);
	if (ret)
		g_hot_mps.errors++;
	return ret;
}
int mi_lb_dealwith_pte_interrupt_cmd(struct lb_command *cmd)
{
	struct lb_pte_record *r;
	struct mm_struct *mm;

	LB_D("mi_lb_dealwith_pte_interrupt_cmd", 0x308, "uid: %d, pid: %d, packagename: %s",
	     cmd->uid, cmd->pid, cmd->name);
	r = mps_find_get_record(cmd->pid, false);
	if (!r) {
		LB_E("mi_lb_dealwith_pte_interrupt_cmd", 0x30c, "mps_rec %u maybe no snapshot", cmd->pid);
		return -2;
	}
	LB_D("mi_lb_dealwith_pte_interrupt_cmd", 0x310,
	     "uid: %d, pid: %d, packagename: %s, interrupt preread", cmd->uid, cmd->pid, cmd->name);
	r->interrupt = 1;
	atomic64_inc(&g_hot_mps.interrupt_ioctl);
	mm = r->mm;
	mps_put_record(r);
	spin_lock(&g_hot_mps.lock);
	mps_unlink_rec_locked(mm);
	spin_unlock(&g_hot_mps.lock);
	return 0;
}
int mi_lb_dealwith_pte_clean_cmd(struct lb_command *cmd)
{
	struct lb_pte_record *r, *next;

	spin_lock(&g_hot_mps.lock);
	list_for_each_entry_safe(r, next, &g_hot_mps.records, list) {
		if (r->pid == cmd->pid && r->uid == cmd->uid) {
			mps_unlink_rec_locked(r->mm);
			break;
		}
	}
	spin_unlock(&g_hot_mps.lock);
	return 0;
}
static void mps_rec_mmput_hook(void *unused, struct mm_struct *mm)
{
	if (mm) {
		spin_lock(&g_hot_mps.lock);
		mps_unlink_rec_locked(mm);
		spin_unlock(&g_hot_mps.lock);
	}
}
int lb_hot_init(void)
{
	LB_I("lb_hot_init", 0x35c, "init");
	memset(&g_hot_mps, 0, sizeof(g_hot_mps));
	spin_lock_init(&g_hot_mps.lock);
	g_hot_mps.segment_cache = kmem_cache_create("pte_segment", 64, 8, SLAB_ACCOUNT, NULL);
	if (!g_hot_mps.segment_cache)
		return -ENOMEM;
	INIT_LIST_HEAD(&g_hot_mps.records);
	xa_init(&g_hot_mps.by_mm);
	g_hot_mps.min_time = S64_MAX;
	/* Stock ignores the mmput hook registration return. */
	register_trace_android_vh_mmput_mm(mps_rec_mmput_hook, NULL);
	return 0;
}
void lb_hot_exit(void)
{
	struct lb_pte_record *r, *next;

	LB_I("lb_hot_exit", 0x365, "exit");
	unregister_trace_android_vh_mmput_mm(mps_rec_mmput_hook, NULL);
	spin_lock(&g_hot_mps.lock);
	list_for_each_entry_safe(r, next, &g_hot_mps.records, list)
		mps_unlink_rec_locked(r->mm);
	spin_unlock(&g_hot_mps.lock);
	rcu_barrier();
	kmem_cache_destroy(g_hot_mps.segment_cache);
}
int lb_hot_collect_show(struct seq_file *s, void *unused)
{
	struct lb_pte_record *r;
	s64 count = atomic64_read(&g_hot_mps.total_count);
	int i = 0;

	if ((u64)count < 2)
		count = 1;
	seq_printf(s, "######################################################################\n"
		"min_swapin, time: %lld, app: %s\nmax_swapin, time: %lld, app: %s\n"
		"avg time, rlock: %lld, swapin: %lld, fadvise: %lld, total: %lld, total count: %lld\n"
		"interrupt, by ioctl: %lld, by rlock: %lld, by swapin: %lld\n"
		"memory usage, pte_segment: %u (%luMB), pte_record: %u (%luMB)\n"
		"######################################################################\n",
		g_hot_mps.min_time == S64_MAX ? 0 : g_hot_mps.min_time, g_hot_mps.min_name,
		g_hot_mps.max_time, g_hot_mps.max_name,
		atomic64_read(&g_hot_mps.rlock_time) / count, atomic64_read(&g_hot_mps.swapin_time) / count,
		atomic64_read(&g_hot_mps.fadvise_time) / count, atomic64_read(&g_hot_mps.total_time) / count,
		atomic64_read(&g_hot_mps.total_count), atomic64_read(&g_hot_mps.interrupt_ioctl),
		atomic64_read(&g_hot_mps.interrupt_rlock), atomic64_read(&g_hot_mps.interrupt_swapin),
		atomic_read(&g_hot_mps.segment_count), ((unsigned long)(s64)atomic_read(&g_hot_mps.segment_count) >> 14) & 0xfffffffffffUL,
		atomic_read(&g_hot_mps.record_count), (unsigned long)((s64)atomic_read(&g_hot_mps.record_count) * 0x128) >> 20);
	spin_lock(&g_hot_mps.lock);
	if (list_empty(&g_hot_mps.records)) {
		seq_puts(s, "g_hot_record_list_user0 is empty\n");
	} else {
		seq_printf(s, "snapshot: %u prepage: %u err: %u\n", g_hot_mps.active_count,
			   g_hot_mps.preread_count, g_hot_mps.errors);
		seq_printf(s, "anon: %llu pre_anon: %llu file: %llu pre_file: %llu\n",
			   g_hot_mps.anon_pages, g_hot_mps.pre_anon, g_hot_mps.file_pages, g_hot_mps.pre_file);
		list_for_each_entry(r, &g_hot_mps.records, list)
			seq_printf(s, "[%d] uid:%d pid:%d , segs %u pages %u %u pre %u %u hit %u %u\n",
				i++, r->uid, r->pid, r->segment_count, r->anon_pages, r->file_pages,
				r->pre_anon, r->pre_file, r->hit_anon, r->hit_file);
	}
	spin_unlock(&g_hot_mps.lock);
	return 0;
}
