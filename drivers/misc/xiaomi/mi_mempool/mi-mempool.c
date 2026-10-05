// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reconstructed from dash OS3.0.305 mi_mempool.ko, 6.6.118.
 * Stock section offsets are evidence references, never kernel struct offsets.
 * See RECONSTRUCTION.txt in this directory for evidence and defects
 * deliberately preserved at the user's request.
 */
#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/gfp.h>
#include <linux/kasan-enabled.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/sysfs.h>
#include <linux/tracepoint.h>
#include <linux/vmstat.h>
#include <asm/mte.h>
#include <trace/hooks/mm.h>

#define POOL_COUNT 4
#define PROCESS_COUNT 15
#define COMM_COUNT 10
#define PID_COUNT 5
#define ORDER_COUNT 11
#define CONFIG_USAGE BIT(0)
#define CONFIG_DEBUG BIT(1)
#define CONFIG_TREE_DEBUG BIT(2)
/* Shared with mi_mem_epoll's reclaim rescue marker, stock task +0xdd8. */
#define RECLAIM_OEM_SLOT 3
#define RECLAIM_RESCUE_FLAG BIT_ULL(0)
/* mm/internal.h private ALLOC_RESERVES, exact bits consumed by stock. */
#define MI_ALLOC_RESERVES 0x238

struct mi_process_stat {
	atomic_t reclaim_success;
	atomic_t reclaim_failure;
	atomic_t oom_success;
	atomic_t oom_failure;
};

struct mi_dynamic_page_pool {
	spinlock_t lock;                 /* stock +0x00 */
	gfp_t gfp_mask;
	unsigned int order;
	struct list_head pages;          /* stock +0x10 */
	atomic_t count;                  /* stock +0x20 */
	atomic_t reclaim_use;
	atomic_t oom_use;
	int low_mark;
	int high_mark;
	int direct_reclaim_mark;
	ktime_t last_watermark_check;    /* stock +0x38 */
	int oom_low_mark[PROCESS_COUNT]; /* stock +0x40 */
	bool initialized;
	int underfilled_refills;
	struct mi_process_stat stats[PROCESS_COUNT]; /* stock +0x84 */
};

struct mi_mempool {
	struct mi_dynamic_page_pool **pools;
	struct task_struct *thread;
	s64 last_refill_ms;
	int last_refill_pages;
	int refill_count;
	unsigned long try_to_free_pages_threshold;
	unsigned long direct_reclaim_threshold;
	bool refill_pending;
	bool free_page_wakeup;
	spinlock_t wakeup_lock;
	bool thread_woken;
	int config;
	struct kobject *kobj;
	atomic_t total_page_num;
	atomic_t oom_use_failure_count[ORDER_COUNT];
	atomic_t tree_lookups;           /* stock +0x78 */
	atomic_t tree_alloc_failures;
	int tree_nodes;
};

struct mi_proc_node {
	struct rb_node rb;
	pid_t pid;
	int process_id;
};

static struct mi_mempool g_mi_mempool;
static struct mi_mempool *pg_mi_mempool = &g_mi_mempool;
static struct rb_root proc_tree = RB_ROOT;
static DEFINE_SPINLOCK(proc_tree_lock);
static struct kmem_cache *proc_node_cachep;
static const unsigned int orders[POOL_COUNT] = { 0, 1, 4, 5 };
/* .rodata.cst16+0x20, including __GFP_ZERO and excluding __GFP_COMP. */
static const gfp_t order_flags[POOL_COUNT] = {
	(__force gfp_t)0x102dc2, (__force gfp_t)0x1121c2,
	(__force gfp_t)0x1121c2, (__force gfp_t)0x1121c2,
};
static int high_mark_mb[POOL_COUNT] = { 64, 1, 1, 1 };
static const int direct_reclaim_mark_mb[POOL_COUNT] = { 17, 1, 1, 1 };
static int order_map[ORDER_COUNT];
static const char * const comm_whitelist[] = {
	"composer", "surfaceflinger", "com.miui.home", "ndroid.systemui",
	"system_server", ".globallauncher", "droid.bluetooth",
	"comm_direct_reclaim", "bypass_throttle", "app_main", NULL,
};
static int comm_whitelist_sizes[COMM_COUNT];
static const int adj_whitelist[11] = {
	-1000, -1000, -2000, -2000, -900, -2000,
	-2000, -3000, -3000, -2000, -2000,
};
static pid_t pid_whitelist[PID_COUNT];
static bool invalid_pid_whitelist[PID_COUNT];
static int system_server_id = -1;
static int system_server_key_thread_low_mark = -1;
static const struct {
	int order;
	int process;
	int percentage;
} oom_low_mark_policys[] = {
	{ 1, -1, 80 }, { 0, 4, 15 }, { 0, 7, 70 },
	{ 0, 9, 80 }, { 4, -1, 100 }, { 5, -1, 100 },
};

extern void mempool_mte_zero_clear_page_tags(void *addr);

/* Preserve the stock compiler's fatal out-of-bounds trap, not a warning. */
static __always_inline void stock_bounds_check(bool invalid)
{
	if (unlikely(invalid)) {
		asm volatile("brk #0x5512");
		unreachable();
	}
}

static bool enabled(unsigned int flag)
{
	return pg_mi_mempool->config & flag;
}

static s64 now_ms(void)
{
	return ktime_to_ns(ktime_get()) / 1000000;
}

/* Stock predates the active tree's extended gfp_zone() virtual-zone policy. */
static unsigned int stock_gfp_zone(gfp_t gfp)
{
	return (0x250015U >> (((__force unsigned int)gfp & 0xf) * 2)) & 3;
}

/* Stock .text+0xcf4, emits the same ftrace marker body. */
static void tracing_mark_write(const char *text)
{
	__trace_puts((unsigned long)tracing_mark_write, text, strlen(text));
}

static struct mi_proc_node *find_proc_node_locked(pid_t pid)
{
	struct rb_node *rb = proc_tree.rb_node;

	while (rb) {
		struct mi_proc_node *node = rb_entry(rb, struct mi_proc_node, rb);

		/* Stock uses rb_right for smaller keys (.text+0x44/+0x2cc4). */
		if (pid < node->pid)
			rb = rb->rb_right;
		else if (pid > node->pid)
			rb = rb->rb_left;
		else
			return node;
	}
	return NULL;
}

static struct mi_proc_node *del_proc_node_by_pid_locked(pid_t pid)
{
	struct mi_proc_node *node = find_proc_node_locked(pid);

	if (node) {
		rb_erase(&node->rb, &proc_tree);
		pg_mi_mempool->tree_nodes--;
	}
	return node;
}

static void insert_new_proc_node(pid_t pid, int process_id)
{
	struct mi_proc_node *node, *old;
	struct rb_node **link = &proc_tree.rb_node, *parent = NULL;
	unsigned long flags;

	if (!proc_node_cachep)
		goto failed;
	node = kmem_cache_alloc(proc_node_cachep, GFP_ATOMIC);
	if (!node)
		goto failed;
	node->pid = pid;
	node->process_id = process_id;
	spin_lock_irqsave(&proc_tree_lock, flags);
	while (*link) {
		parent = *link;
		old = rb_entry(parent, struct mi_proc_node, rb);
		if (pid < old->pid)
			link = &parent->rb_right;
		else if (pid > old->pid)
			link = &parent->rb_left;
		else {
			if (proc_node_cachep)
				kmem_cache_free(proc_node_cachep, node);
			goto out;
		}
	}
	rb_link_node(&node->rb, parent, link);
	rb_insert_color(&node->rb, &proc_tree);
	pg_mi_mempool->tree_nodes++;
out:
	spin_unlock_irqrestore(&proc_tree_lock, flags);
	return;
failed:
	if (enabled(CONFIG_TREE_DEBUG))
		atomic_inc(&pg_mi_mempool->tree_alloc_failures);
}

/* Stock .text+0xefc. Cache invalidation deliberately occurs only on task free. */
static int check_whitelist(void)
{
	struct mi_dynamic_page_pool *pool;
	struct mi_proc_node *node;
	struct task_struct *leader = current->group_leader;
	unsigned long flags;
	int process_id = 0, threshold, i;
	bool rescue;

	if (enabled(CONFIG_TREE_DEBUG))
		atomic_inc(&pg_mi_mempool->tree_lookups);
	pool = order_map[0] == -1 ? NULL : pg_mi_mempool->pools[order_map[0]];
	threshold = pool->high_mark * 70 / 100;
	rescue = current->android_oem_data1[RECLAIM_OEM_SLOT] & RECLAIM_RESCUE_FLAG;
	if (enabled(CONFIG_TREE_DEBUG))
		pr_info("mi_mempool: cold_start_secne %d bypass_throttle %d comm_usage %d\n",
			0, rescue, threshold);
	if (rescue)
		return 9;
	if (atomic_read(&pool->count) > threshold)
		return 8;
	spin_lock_irqsave(&proc_tree_lock, flags);
	node = find_proc_node_locked(leader->pid);
	if (node) {
		process_id = node->process_id;
		spin_unlock_irqrestore(&proc_tree_lock, flags);
		return process_id;
	}
	spin_unlock_irqrestore(&proc_tree_lock, flags);
	for (i = 0; i < COMM_COUNT; i++) {
		if (i >= 7)
			continue;
		if (!(BIT(i) & 0x66c) &&
		    adj_whitelist[i] != current->signal->oom_score_adj)
			continue;
		if (leader && !strncmp(leader->comm, comm_whitelist[i],
				       comm_whitelist_sizes[i])) {
			process_id = i + 1;
			goto insert;
		}
	}
	for (i = 0; i < PID_COUNT; i++) {
		if (!invalid_pid_whitelist[i] && leader &&
		    leader->pid == pid_whitelist[i]) {
			process_id = COMM_COUNT + i + 1;
			break;
		}
	}
insert:
	/* Stock dereferences leader even after its earlier NULL checks. */
	insert_new_proc_node(leader->pid, process_id);
	return process_id;
}

/* Stock zone snapshots use online CPUs 0..31 (CONFIG_NR_CPUS=32). */
static unsigned long mi_zone_page_state_snapshot(struct zone *zone,
					       enum zone_stat_item item)
{
	long value = atomic_long_read(&zone->vm_stat[item]);
	int cpu;

	for_each_online_cpu(cpu) {
		if (cpu >= 32)
			break;
		value += per_cpu_ptr(zone->per_cpu_zonestats, cpu)->vm_stat_diff[item];
	}
	return value < 0 ? 0 : value;
}

static unsigned long zone_can_reclaimable_pages(struct zone *zone)
{
	unsigned long pages;

	pages = mi_zone_page_state_snapshot(zone, NR_ZONE_INACTIVE_FILE) +
		mi_zone_page_state_snapshot(zone, NR_ZONE_ACTIVE_FILE);
	if (atomic_long_read(&nr_swap_pages) > 0)
		pages += mi_zone_page_state_snapshot(zone, NR_ZONE_INACTIVE_ANON) +
			mi_zone_page_state_snapshot(zone, NR_ZONE_ACTIVE_ANON);
	return pages;
}

static void get_watermarks(int *minimum, int *low, int *high)
{
	int z;

	*minimum = *low = *high = 0;
	/* Stock explicitly samples zone indices 0 and 1. */
	for (z = 0; z < 2; z++) {
		struct zone *zone = &NODE_DATA(0)->node_zones[z];

		if (!zone_managed_pages(zone) || !zone_can_reclaimable_pages(zone))
			continue;
		*minimum += (int)zone->_watermark[WMARK_MIN] + (int)zone->watermark_boost;
		*low += (int)zone->_watermark[WMARK_LOW] + (int)zone->watermark_boost;
		*high += (int)zone->_watermark[WMARK_HIGH] + (int)zone->watermark_boost;
	}
}

static struct page *mi_dynamic_page_pool_remove(struct mi_dynamic_page_pool *pool)
{
	struct page *page;
	unsigned long flags;

	spin_lock_irqsave(&pool->lock, flags);
	page = list_first_entry_or_null(&pool->pages, struct page, lru);
	if (page) {
		list_del(&page->lru);
		mod_node_page_state(NODE_DATA(0), NR_KERNEL_MISC_RECLAIMABLE,
				    -(1L << pool->order));
	}
	spin_unlock_irqrestore(&pool->lock, flags);
	if (page) {
		atomic_dec(&pool->count);
		atomic_sub(1 << pool->order, &pg_mi_mempool->total_page_num);
	}
	return page;
}

static void mi_dynamic_page_pools_destory(void)
{
	int i;

	for (i = 0; i < POOL_COUNT; i++) {
		struct mi_dynamic_page_pool *pool = pg_mi_mempool->pools[i];
		struct page *page;

		while ((page = mi_dynamic_page_pool_remove(pool)))
			__free_pages(page, pool->order);
		kfree(pool);
	}
	/* Stock .text+0x2304 does not free the array or clear any pointers. */
}

static void wake_refill_thread(void)
{
	unsigned long flags;

	spin_lock_irqsave(&pg_mi_mempool->wakeup_lock, flags);
	if (!pg_mi_mempool->thread_woken) {
		pg_mi_mempool->thread_woken = true;
		wake_up_process(pg_mi_mempool->thread);
	}
	spin_unlock_irqrestore(&pg_mi_mempool->wakeup_lock, flags);
}

static void prepare_pool_page(struct page *page, unsigned int order, gfp_t gfp)
{
	int i;
	bool skip_kasan = !kasan_hw_tags_enabled() || (gfp & __GFP_SKIP_KASAN);

	/* .text+0x930 jump-table key restores the KASAN-enabled branch. */
	if (skip_kasan) {
		clear_bit(PG_mte_lock, &page->flags);
		clear_bit(PG_mte_tagged, &page->flags);
		for (i = 0; i < (1 << order); i++) {
			struct page *p = page + i;

			if (gfp & __GFP_ZEROTAGS) {
				bool tagged = try_page_mte_tagging(p);

				/* Stock WARN_ON after waiting if the tag lock was held. */
				WARN_ON(!tagged);
				mempool_mte_zero_clear_page_tags(page_address(p));
				set_page_mte_tagged(p);
			}
			page_kasan_tag_reset(p);
		}
	}
	if (order && (gfp & __GFP_COMP))
		/* Active exported implementation has the stock field effects. */
		prep_compound_page(page, order);
}

static struct page *mi_dynamic_page_pool_alloc(unsigned int order,
					      struct mi_dynamic_page_pool *pool,
					      gfp_t gfp, bool oom)
{
	struct page *page;

	if (current == pg_mi_mempool->thread ||
	    stock_gfp_zone(gfp) < stock_gfp_zone(pool->gfp_mask))
		return NULL;
	page = mi_dynamic_page_pool_remove(pool);
	if (page) {
		atomic_inc(oom ? &pool->oom_use : &pool->reclaim_use);
		prepare_pool_page(page, order, gfp);
	}
	if (pg_mi_mempool->thread && atomic_read(&pool->count) <= pool->low_mark &&
	    now_ms() - pg_mi_mempool->last_refill_ms >= 300 &&
	    !pg_mi_mempool->thread_woken)
		wake_refill_thread();
	return page;
}

static void mi_mempool_alloc_oom(void *unused, gfp_t gfp, int order,
				int alloc_flags, int migratetype, struct page **page)
{
	struct mi_dynamic_page_pool *pool;
	int index, process_id;

	if (!(gfp & __GFP_NOWARN) && order < ORDER_COUNT) {
		stock_bounds_check((unsigned int)order >= ORDER_COUNT);
		index = order_map[order];
		pool = index == -1 ? NULL : pg_mi_mempool->pools[index];
		if (pool) {
			if (enabled(CONFIG_DEBUG))
				pr_info("mi_mempool alloc_oom comm=%s pid=%d order=%d gfp_mask:%#x(%pGg)\n",
					current->comm, current->pid, order, gfp, &gfp);
			*page = mi_dynamic_page_pool_alloc(order, pool, gfp, true);
			if (enabled(CONFIG_USAGE)) {
				process_id = check_whitelist();
				if (process_id) {
					stock_bounds_check((unsigned int)process_id > PROCESS_COUNT);
					atomic_inc(*page ? &pool->stats[process_id - 1].oom_success :
						   &pool->stats[process_id - 1].oom_failure);
				}
			}
		}
	}
	if (!(gfp & __GFP_NOWARN) && !*page) {
		stock_bounds_check((unsigned int)order > ORDER_COUNT);
		/* Stock bound permits order 11 and aliases tree_lookups (+0x78). */
		if (order == ORDER_COUNT)
			atomic_inc(&pg_mi_mempool->tree_lookups);
		else
			atomic_inc(&pg_mi_mempool->oom_use_failure_count[order]);
	}
}

static void mi_mempool_alloc_reclaim(void *unused, gfp_t gfp, int order,
				    int alloc_flags, int migratetype, struct page **page)
{
	struct mi_dynamic_page_pool *pool = NULL;
	int process_id, process_index, count, minimum = 0;
	int free_pages = 0, snapshot_free = 0, cma_free = 0, highatomic = 0;
	int threshold1, threshold2, wmark_result = 0, low_mark;
	bool direct_mark_ok, low_mark_suited, sskey_thread;
	char trace[256] = { 0 };
	int z;

	if (order < ORDER_COUNT) {
		stock_bounds_check((unsigned int)order >= ORDER_COUNT);
		if (order_map[order] != -1)
			pool = pg_mi_mempool->pools[order_map[order]];
	}
	process_id = check_whitelist();
	if (!process_id || !pool)
		return;
	if (!(current->flags & PF_KTHREAD) && !fatal_signal_pending(current)) {
		for (z = 0; z < 2; z++) {
			struct zone *zone = &NODE_DATA(0)->node_zones[z];

			if (!zone_managed_pages(zone) || !zone_can_reclaimable_pages(zone))
				continue;
			minimum += (int)zone->_watermark[WMARK_MIN] +
				(int)zone->watermark_boost;
			free_pages += (int)zone_page_state(zone, NR_FREE_PAGES);
			if (!(gfp & __GFP_CMA))
				cma_free += (int)zone_page_state(zone, NR_FREE_CMA_PAGES);
			if (!(alloc_flags & MI_ALLOC_RESERVES))
				highatomic += (int)zone->nr_reserved_highatomic;
			if (enabled(CONFIG_DEBUG))
				snapshot_free += (int)mi_zone_page_state_snapshot(zone, NR_FREE_PAGES);
		}
		threshold1 = (int)pg_mi_mempool->try_to_free_pages_threshold;
		/* Stock actually uses +944 pages; sysfs text says min_plus_10mb. */
		threshold2 = max(minimum + 0x3b0, threshold1);
		if ((int)pg_mi_mempool->direct_reclaim_threshold != -1)
			threshold2 = (int)pg_mi_mempool->direct_reclaim_threshold;
		wmark_result = free_pages - (cma_free + highatomic) <= threshold2;
		if (enabled(CONFIG_DEBUG)) {
			snprintf(trace, sizeof(trace),
				 "I|%d|mi_pool: check_wmark_result:%d o:%d free:%d sfree:%d cma_free:%d high_free:%d threshold1:%d threshold2:%d",
				 task_tgid_nr(current), free_pages <= threshold1 ? 2 : wmark_result,
				 order, free_pages, snapshot_free, cma_free, highatomic,
				 threshold1, threshold2);
			tracing_mark_write(trace);
		}
		if (free_pages <= threshold1) {
			direct_mark_ok = true;
			wmark_result = 2;
		} else {
			direct_mark_ok = atomic_read(&pool->count) > pool->direct_reclaim_mark;
		}
	} else {
		direct_mark_ok = atomic_read(&pool->count) > pool->direct_reclaim_mark;
	}
	process_index = process_id - 1;
	stock_bounds_check((unsigned int)process_index >= PROCESS_COUNT);
	count = atomic_read(&pool->count);
	low_mark = pool->oom_low_mark[process_index];
	low_mark_suited = count > low_mark;
	sskey_thread = !order && process_index == system_server_id && !low_mark_suited;
	if (sskey_thread) {
		if (!strncmp(current->comm, "MiuiIntegratedM", 15) ||
		    !strncmp(current->comm, "SystemPressureC", 15)) {
			low_mark_suited = count > system_server_key_thread_low_mark;
		} else {
			sskey_thread = false;
			low_mark_suited = false;
		}
	}
	if (direct_mark_ok && low_mark_suited) {
		*page = mi_dynamic_page_pool_alloc(order, pool, gfp, false);
		low_mark_suited = true;
	}
	if (enabled(CONFIG_DEBUG)) {
		pr_info("mi_pool: o:%d wmark_check_result:%d dr_mark_ok:%d count:%d dr_mark:%d low_mark_suited:%d sskey_thread:%d sskey_thread_mark:%d scucess:%d process_id:%d\n",
			order, wmark_result, direct_mark_ok, atomic_read(&pool->count),
			pool->direct_reclaim_mark, low_mark_suited, sskey_thread,
			system_server_key_thread_low_mark, !!*page, process_id);
		memset(trace, 0, sizeof(trace));
		snprintf(trace, sizeof(trace),
			 "I|%d|mi_pool: o:%d wmark_check_result:%d dr_mark_ok:%d count:%d dr_mark:%d low_mark_suited:%d sskey_thread:%d sskey_thread_mark:%d scucess:%d process_id %d",
			 task_tgid_nr(current), order, wmark_result, direct_mark_ok,
			 atomic_read(&pool->count), pool->direct_reclaim_mark,
			 low_mark_suited, sskey_thread, system_server_key_thread_low_mark,
			 !!*page, process_id);
		tracing_mark_write(trace);
	}
	if (enabled(CONFIG_USAGE))
		atomic_inc(*page ? &pool->stats[process_index].reclaim_success :
			   &pool->stats[process_index].reclaim_failure);
}

static void mi_mempool_free_task(void *unused, struct task_struct *task)
{
	struct mi_proc_node *node;
	unsigned long flags;
	int i;

	if (task->pid != task->tgid)
		return;
	spin_lock_irqsave(&proc_tree_lock, flags);
	node = del_proc_node_by_pid_locked(task->pid);
	spin_unlock_irqrestore(&proc_tree_lock, flags);
	if (node) {
		if (proc_node_cachep)
			kmem_cache_free(proc_node_cachep, node);
		if (enabled(CONFIG_DEBUG))
			pr_info("mi_mempool free_task comm=%s pid=%d\n", task->comm, task->pid);
	}
	for (i = 0; i < PID_COUNT; i++) {
		if (pid_whitelist[i] == task->pid) {
			invalid_pid_whitelist[i] = true;
			break;
		}
	}
}

static void mi_mempool_refill_check(void *unused, struct page *page, int order,
				   int migratetype, bool *bypass)
{
	unsigned long flags;

	if (now_ms() - pg_mi_mempool->last_refill_ms < 300 ||
	    !pg_mi_mempool->refill_pending || pg_mi_mempool->thread_woken)
		return;
	spin_lock_irqsave(&pg_mi_mempool->wakeup_lock, flags);
	if (!pg_mi_mempool->thread_woken) {
		pg_mi_mempool->free_page_wakeup = true;
		pg_mi_mempool->thread_woken = true;
		wake_up_process(pg_mi_mempool->thread);
	}
	spin_unlock_irqrestore(&pg_mi_mempool->wakeup_lock, flags);
}

/* Inlined stock refill watermark algorithm, .text+0x2544..0x2774. */
static bool check_watermark(struct mi_dynamic_page_pool *pool)
{
	int highest = stock_gfp_zone(pool->gfp_mask), z, order;

	for (z = highest; z >= 0; z--) {
		struct zone *zone = &NODE_DATA(0)->node_zones[z];
		long free, mark;

		if (!strcmp(zone->name, "DMA32"))
			continue;
		free = zone_page_state(zone, NR_FREE_PAGES);
		if (zone->percpu_drift_mark && free < zone->percpu_drift_mark)
			free = mi_zone_page_state_snapshot(zone, NR_FREE_PAGES);
		if (!zone_is_suitable(zone, pool->order))
			continue;
		mark = zone->_watermark[WMARK_LOW] + zone->watermark_boost +
			(1 << pool->order);
		free -= zone->nr_reserved_highatomic + ((1 << pool->order) - 1) +
			zone_page_state(zone, NR_FREE_CMA_PAGES);
		if (free <= mark + zone->lowmem_reserve[highest])
			continue;
		if (!pool->order)
			goto passed;
		for (order = pool->order; order < ORDER_COUNT; order++) {
			struct free_area *area = &zone->free_area[order];
			int mt;

			if (!area->nr_free)
				continue;
			/* Stock checks the first four lists, including HIGHATOMIC. */
			for (mt = 0; mt < 4; mt++)
				if (!list_empty(&area->free_list[mt]))
					goto passed;
		}
		continue;
passed:
		if (enabled(CONFIG_DEBUG))
			pr_info("mi_mempool %s name=%s pool->order=%d mark=%lu free_pages=%lu highatomic=%lu cma=%lu\n",
				"check_watermark", zone->name, pool->order, mark,
				zone_page_state(zone, NR_FREE_PAGES),
				zone->nr_reserved_highatomic,
				zone_page_state(zone, NR_FREE_CMA_PAGES));
		return true;
	}
	return false;
}

static int mi_mempool_refill_work(void *unused)
{
	struct mi_dynamic_page_pool **pools = pg_mi_mempool->pools;
	char trace[300] = { 0 };
	unsigned long flags;
	int i, len, added;

	for (;;) {
		int config = pg_mi_mempool->config;

		pg_mi_mempool->last_refill_ms = now_ms();
		pg_mi_mempool->refill_pending = false;
		pg_mi_mempool->last_refill_pages = 0;
		pg_mi_mempool->refill_count++;
		len = 0;
		if (config & CONFIG_DEBUG)
			len = scnprintf(trace, sizeof(trace), "I|%d|mi_pool: refill ",
					task_tgid_nr(current));
		for (i = 0; i < POOL_COUNT; i++) {
			struct mi_dynamic_page_pool *pool = pools[i];

			added = 0;
			if (pool->initialized && atomic_read(&pool->count) > pool->low_mark)
				goto trace_order;
			while (atomic_read(&pool->count) < pool->high_mark) {
				struct page *page;

				if (ktime_to_ns(ktime_get()) - pool->last_watermark_check >= 10000000) {
					if (!check_watermark(pool))
						break;
					pool->last_watermark_check = ktime_get();
				}
				page = __alloc_pages(pool->gfp_mask, pool->order, 0, NULL);
				if (!page)
					break;
				spin_lock_irqsave(&pool->lock, flags);
				list_add(&page->lru, &pool->pages);
				mod_node_page_state(NODE_DATA(0), NR_KERNEL_MISC_RECLAIMABLE,
						    1 << pool->order);
				spin_unlock_irqrestore(&pool->lock, flags);
				atomic_inc(&pool->count);
				atomic_add(1 << pool->order, &pg_mi_mempool->total_page_num);
				added++;
			}
			if (!pool->initialized) {
				if (atomic_read(&pool->count) < pool->high_mark) {
					pg_mi_mempool->refill_pending = true;
					pool->underfilled_refills++;
					if (pool->underfilled_refills > 15)
						pool->initialized = true;
				} else {
					pool->initialized = true;
				}
			} else if (atomic_read(&pool->count) <= pool->low_mark) {
				pg_mi_mempool->refill_pending = true;
			}
			added <<= pool->order;
			pg_mi_mempool->last_refill_pages += added;
trace_order:
			if (enabled(CONFIG_DEBUG) && len < sizeof(trace))
				len += scnprintf(trace + len, sizeof(trace) - len,
						 "order-%d:%d ", pool->order, added);
		}
		if (enabled(CONFIG_DEBUG)) {
			stock_bounds_check(len > sizeof(trace));
			len += scnprintf(trace + len, sizeof(trace) - len,
					 " cost_ms=%lld wakeup=%d",
					 now_ms() - pg_mi_mempool->last_refill_ms,
					 pg_mi_mempool->free_page_wakeup);
			if (len >= sizeof(trace))
				len--;
			stock_bounds_check(len >= sizeof(trace));
			trace[len] = '\0';
			tracing_mark_write(trace);
		}
		pg_mi_mempool->free_page_wakeup = false;
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			set_current_state(TASK_RUNNING);
			return 0;
		}
		spin_lock_irqsave(&pg_mi_mempool->wakeup_lock, flags);
		pg_mi_mempool->thread_woken = false;
		spin_unlock_irqrestore(&pg_mi_mempool->wakeup_lock, flags);
		schedule();
		set_current_state(TASK_RUNNING);
	}
}

static void free_all_proc_nodes_and_caches(void)
{
	struct mi_proc_node *node, *next;
	unsigned long flags;

	spin_lock_irqsave(&proc_tree_lock, flags);
	rbtree_postorder_for_each_entry_safe(node, next, &proc_tree, rb)
		if (proc_node_cachep)
			kmem_cache_free(proc_node_cachep, node);
	proc_tree = RB_ROOT;
	spin_unlock_irqrestore(&proc_tree_lock, flags);
	kmem_cache_destroy(proc_node_cachep);
}

static ssize_t config_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", pg_mi_mempool->config);
}

static ssize_t config_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	int value = 0;

	if (kstrtoint(buf, 0, &value) < 0)
		return -EINVAL;
	pg_mi_mempool->config = value;
	return count;
}

static ssize_t pids_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int len = 0, i;

	for (i = 0; i < PID_COUNT; i++)
		len += snprintf(buf + len, PAGE_SIZE - len, "%d-%d ",
				pid_whitelist[i], invalid_pid_whitelist[i]);
	len += snprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

static ssize_t pids_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	bool remove = buf[0] == '-';
	int pid = 0, i;

	if (kstrtoint(buf + remove, 0, &pid) < 0)
		return -EINVAL;
	if (pid < 1) {
		pr_info("mi_mempool pid OVF! pid:%d\n", pid);
		return count;
	}
	if (remove) {
		for (i = 0; i < PID_COUNT; i++) {
			if (pid_whitelist[i] == pid) {
				invalid_pid_whitelist[i] = true;
				break;
			}
		}
		return count;
	}
	for (i = 0; i < PID_COUNT; i++) {
		if (invalid_pid_whitelist[i]) {
			pid_whitelist[i] = pid;
			invalid_pid_whitelist[i] = false;
			pr_info("pid:%d\n", pid);
			return count;
		}
	}
	pr_err("mi_mempool pid_whitelist full! add pid:%d to whitelist failed!\n", pid);
	return count;
}

/* Preserve stock 32-bit shift followed by sign extension and unsigned shift. */
static unsigned long mb_to_pages(int mb)
{
	return (unsigned long)(long)(s32)((u32)mb << 20) >> PAGE_SHIFT;
}

static ssize_t direct_reclaim_threshold_mb_store(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	int id = 0, mb = 0;

	if (sscanf(buf, "%d %d", &id, &mb) != 2)
		return -EINVAL;
	if (id == 1) {
		if (mb < -1 || (mb >= 0 &&
		    mb < ((pg_mi_mempool->try_to_free_pages_threshold >> 8) & 0xfffffffffffUL)))
			return -EINVAL;
		pg_mi_mempool->direct_reclaim_threshold = mb == -1 ?
			~0UL : mb_to_pages(mb);
	} else if (id == 0) {
		if (mb < 0)
			return -EINVAL;
		pg_mi_mempool->try_to_free_pages_threshold = mb_to_pages(mb);
	}
	/* Unknown ids return success without changing either threshold in stock. */
	return count;
}

static ssize_t direct_reclaim_threshold_mb_show(struct kobject *kobj,
					      struct kobj_attribute *attr, char *buf)
{
	int minimum, low, high, len;
	int direct = pg_mi_mempool->direct_reclaim_threshold;

	get_watermarks(&minimum, &low, &high);
	len = snprintf(buf, PAGE_SIZE,
		"# echo format: echo 'id threshold_mb' > direct_reclaim_threshold_mb\n"
		"# id = 0, modify try_to_free_pages_threshold, id = 1, modify direct_reclaim_threshold\n"
		"# if try_to_free_pages_threshold == -1, then use max(try_to_free_pages_threshold, min_plus_10mb)\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "try_to_free_pages_threshold:%dmb\n",
			(int)(pg_mi_mempool->try_to_free_pages_threshold >> 8));
	if (direct >= 0)
		direct = (pg_mi_mempool->direct_reclaim_threshold >> 8) & 0xffffff;
	len += snprintf(buf + len, PAGE_SIZE - len, "direct_reclaim_threshold:%dmb\n", direct);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"min:%dmb, low:%dmb, high:%dmb, min_plus_10mb:%dmb\n",
			minimum >> 8, low >> 8, high >> 8, (minimum + 0xa00) >> 8);
	return len;
}

static ssize_t process_use_count_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	int p, i, len = 0;

	for (p = 0; p < PROCESS_COUNT; p++) {
		if (p < COMM_COUNT) {
			len += snprintf(buf + len, PAGE_SIZE - len, "comm:%s\n", comm_whitelist[p]);
		} else {
			if (invalid_pid_whitelist[p - COMM_COUNT])
				continue;
			len += snprintf(buf + len, PAGE_SIZE - len, "comm:pid-%d\n",
					pid_whitelist[p - COMM_COUNT]);
		}
		for (i = 0; i < POOL_COUNT; i++) {
			struct mi_dynamic_page_pool *pool = pg_mi_mempool->pools[i];
			struct mi_process_stat *stat = &pool->stats[p];

			len += snprintf(buf + len, PAGE_SIZE - len, "order-%d:%d %d %d %d %d\n",
				pool->order, atomic_read(&stat->reclaim_success),
				atomic_read(&stat->reclaim_failure), atomic_read(&stat->oom_success),
				atomic_read(&stat->oom_failure), pool->oom_low_mark[p]);
		}
	}
	return len;
}

static ssize_t general_stat_info_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	int i, len = 0, failures = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "total_page_num: %d\n",
			atomic_read(&pg_mi_mempool->total_page_num));
	len += snprintf(buf + len, PAGE_SIZE - len, "last_refill: %lu %d %d\n",
			(unsigned long)pg_mi_mempool->last_refill_ms,
			pg_mi_mempool->last_refill_pages, pg_mi_mempool->refill_count);
	for (i = 0; i < POOL_COUNT; i++) {
		struct mi_dynamic_page_pool *pool = pg_mi_mempool->pools[i];

		len += snprintf(buf + len, PAGE_SIZE - len, "order-%d: %d %d %d %d %d %d\n",
			pool->order, atomic_read(&pool->count), atomic_read(&pool->reclaim_use),
			atomic_read(&pool->oom_use), pool->low_mark,
			pool->direct_reclaim_mark, pool->high_mark);
	}
	len += snprintf(buf + len, PAGE_SIZE - len, "rb_tree: %d %d %d\n",
			atomic_read(&pg_mi_mempool->tree_lookups), pg_mi_mempool->tree_nodes,
			atomic_read(&pg_mi_mempool->tree_alloc_failures));
	for (i = 0; i < ORDER_COUNT; i++) {
		int value = atomic_read(&pg_mi_mempool->oom_use_failure_count[i]);

		failures += value;
		len += snprintf(buf + len, PAGE_SIZE - len,
				i ? " %d:%d" : "oom_use_failure_count: %d:%d", i, value);
	}
	len += snprintf(buf + len, PAGE_SIZE - len, " %d\n", failures);
	len += snprintf(buf + len, PAGE_SIZE - len, "system_server: %d %d\n",
			system_server_id, system_server_key_thread_low_mark);
	return len;
}

static struct kobj_attribute kobj_attr_process_use_count =
	__ATTR(process_use_count, 0440, process_use_count_show, NULL);
static struct kobj_attribute kobj_attr_general_stat_info =
	__ATTR(general_stat_info, 0440, general_stat_info_show, NULL);
static struct kobj_attribute kobj_attr_config =
	__ATTR(config, 0660, config_show, config_store);
static struct kobj_attribute kobj_attr_direct_reclaim_threshold_mb =
	__ATTR(direct_reclaim_threshold_mb, 0660,
	       direct_reclaim_threshold_mb_show, direct_reclaim_threshold_mb_store);
static struct kobj_attribute kobj_attr_pids = __ATTR(pids, 0660, pids_show, pids_store);
static const struct attribute *attrs[] = {
	&kobj_attr_process_use_count.attr, &kobj_attr_general_stat_info.attr,
	&kobj_attr_config.attr, &kobj_attr_direct_reclaim_threshold_mb.attr,
	&kobj_attr_pids.attr, NULL,
};

static int __init mi_mempool_init(void)
{
	struct sysinfo info = { 0 };
	struct kobject *kobj;
	struct task_struct *thread;
	int i, p, ret;

	BUILD_BUG_ON(sizeof(struct mi_dynamic_page_pool) != 0x178);
	BUILD_BUG_ON(offsetof(struct mi_dynamic_page_pool, stats) != 0x84);
	BUILD_BUG_ON(sizeof(struct mi_mempool) != 0x88);
	BUILD_BUG_ON(offsetof(struct mi_mempool, tree_lookups) != 0x78);
	BUILD_BUG_ON(PAGE_SHIFT != 12);
	kobj = kobject_create_and_add("mi_mempool", kernel_kobj);
	if (!kobj) {
		pr_err("failed to create mi_mempool node.\n");
		pg_mi_mempool->kobj = NULL;
		return -1;
	}
	/* Stock exposes sysfs before its pool and slab state is initialized. */
	ret = sysfs_create_files(kobj, attrs);
	if (ret) {
		pr_err("failed to create mi_mempool attrs.\n");
		kobject_put(kobj);
		pg_mi_mempool->kobj = NULL;
		return -1;
	}
	pg_mi_mempool->kobj = kobj;
	si_meminfo(&info);
	if (info.totalram > 0x200000)
		high_mark_mb[0] = 90;
	pg_mi_mempool->config = 0;
	pg_mi_mempool->thread_woken = true;
	pg_mi_mempool->try_to_free_pages_threshold = 0xb00;
	pg_mi_mempool->direct_reclaim_threshold = ~0UL;
	for (i = 0; i < COMM_COUNT; i++) {
		comm_whitelist_sizes[i] = strlen(comm_whitelist[i]);
		if (!strncmp(comm_whitelist[i], "system_server", comm_whitelist_sizes[i]))
			system_server_id = i;
	}
	memset(invalid_pid_whitelist, 1, sizeof(invalid_pid_whitelist));
	/* GFP_KERNEL | __GFP_ZERO matches stock pool-pointer allocation 0xdc0. */
	pg_mi_mempool->pools = kzalloc(sizeof(*pg_mi_mempool->pools) * POOL_COUNT, GFP_KERNEL);
	if (!pg_mi_mempool->pools)
		/* Stock leaves the published sysfs object behind on this failure. */
		return -1;
	memset(order_map, 0xff, sizeof(order_map));
	for (i = 0; i < POOL_COUNT; i++) {
		struct mi_dynamic_page_pool *pool;

		pool = kmalloc(sizeof(*pool), GFP_KERNEL);
		pg_mi_mempool->pools[i] = pool;
		order_map[orders[i]] = i;
		if (!pool)
			goto pool_failure;
		memset(pool, 0, sizeof(*pool));
		spin_lock_init(&pool->lock);
		pool->gfp_mask = order_flags[i];
		pool->order = orders[i];
		INIT_LIST_HEAD(&pool->pages);
		pool->high_mark = mb_to_pages(high_mark_mb[i]) >> pool->order;
		pool->low_mark = pool->high_mark * 20 / 100;
		pool->direct_reclaim_mark = mb_to_pages(direct_reclaim_mark_mb[i]) >> pool->order;
		if (pool->order == 4 || pool->order == 5)
			pool->low_mark = -1;
		if (!pool->order)
			system_server_key_thread_low_mark = pool->high_mark * 15 / 100;
		for (p = 0; p < PROCESS_COUNT; p++)
			pool->oom_low_mark[p] = pool->high_mark * 10 / 100;
	}
	for (i = 0; i < ARRAY_SIZE(oom_low_mark_policys); i++) {
		struct mi_dynamic_page_pool *pool =
			pg_mi_mempool->pools[order_map[oom_low_mark_policys[i].order]];
		int mark = pool->high_mark * oom_low_mark_policys[i].percentage / 100;

		if (oom_low_mark_policys[i].process == -1) {
			for (p = 0; p < PROCESS_COUNT; p++)
				pool->oom_low_mark[p] = mark;
		} else {
			pool->oom_low_mark[oom_low_mark_policys[i].process] = mark;
		}
	}
	proc_node_cachep = kmem_cache_create("proc_node", sizeof(struct mi_proc_node), 0, 0, NULL);
	if (!proc_node_cachep) {
		kobject_put(pg_mi_mempool->kobj);
		pg_mi_mempool->kobj = NULL;
		mi_dynamic_page_pools_destory();
		pr_err("failed to init proc_node mem_cache.\n");
		return -1;
	}
	thread = kthread_create(mi_mempool_refill_work, NULL, "mi_mempool");
	if (IS_ERR(thread)) {
		pg_mi_mempool->thread = thread;
		free_all_proc_nodes_and_caches();
		kobject_put(pg_mi_mempool->kobj);
		pg_mi_mempool->kobj = NULL;
		mi_dynamic_page_pools_destory();
		pr_err("failed to start mi_mempool thread\n");
		return -1;
	}
	/* Stock wakes the thread before publishing the recursion-guard pointer. */
	wake_up_process(thread);
	pg_mi_mempool->thread = thread;
	ret = register_trace_android_vh_alloc_pages_reclaim_bypass(mi_mempool_alloc_reclaim, NULL);
	if (ret) {
		pr_err("Failed to register alloc_pages_reclaim_bypass hook.\n");
		goto hook_failure;
	}
	ret = register_trace_android_vh_alloc_pages_failure_bypass(mi_mempool_alloc_oom, NULL);
	if (ret) {
		pr_err("Failed to register alloc_pages_failure_bypass hook.\n");
		goto unregister_reclaim;
	}
	ret = register_trace_android_vh_free_unref_page_bypass(mi_mempool_refill_check, NULL);
	if (ret) {
		pr_err("Failed to register mmput hook.\n");
		goto unregister_oom;
	}
	ret = register_trace_android_vh_free_task(mi_mempool_free_task, NULL);
	if (ret) {
		pr_err("Failed to register free_task hook.\n");
		goto unregister_free;
	}
	pr_info("mi_mempool init ok\n");
	return 0;
unregister_free:
	unregister_trace_android_vh_free_unref_page_bypass(mi_mempool_refill_check, NULL);
unregister_oom:
	unregister_trace_android_vh_alloc_pages_failure_bypass(mi_mempool_alloc_oom, NULL);
unregister_reclaim:
	unregister_trace_android_vh_alloc_pages_reclaim_bypass(mi_mempool_alloc_reclaim, NULL);
hook_failure:
	/* Preserved: destroy resources before stopping the live refill thread. */
	free_all_proc_nodes_and_caches();
	kobject_put(pg_mi_mempool->kobj);
	pg_mi_mempool->kobj = NULL;
	mi_dynamic_page_pools_destory();
	if (pg_mi_mempool->thread)
		kthread_stop(pg_mi_mempool->thread);
	return -1;
pool_failure:
	for (i = 0; i < POOL_COUNT; i++) {
		struct mi_dynamic_page_pool *pool = pg_mi_mempool->pools[i];
		struct page *page;

		if (!pool)
			continue;
		while ((page = mi_dynamic_page_pool_remove(pool)))
			__free_pages(page, pool->order);
		kfree(pool);
	}
	kfree(pg_mi_mempool->pools);
	/* Preserved: no kobject_put in the stock pool-allocation failure path. */
	return -1;
}

static void __exit mi_mempool_exit(void)
{
	unregister_trace_android_vh_alloc_pages_reclaim_bypass(mi_mempool_alloc_reclaim, NULL);
	unregister_trace_android_vh_alloc_pages_failure_bypass(mi_mempool_alloc_oom, NULL);
	unregister_trace_android_vh_free_unref_page_bypass(mi_mempool_refill_check, NULL);
	unregister_trace_android_vh_free_task(mi_mempool_free_task, NULL);
	free_all_proc_nodes_and_caches();
	if (pg_mi_mempool->kobj) {
		kobject_put(pg_mi_mempool->kobj);
		pg_mi_mempool->kobj = NULL;
	}
	mi_dynamic_page_pools_destory();
	if (pg_mi_mempool->thread)
		kthread_stop(pg_mi_mempool->thread);
}

module_init(mi_mempool_init);
module_exit(mi_mempool_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zhouwenhao");
MODULE_DESCRIPTION("A moudle to preserve some pages for some core apps or memory allocation failures.");
