// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reconstructed from dash OS3.0.305 mi_mem_epoll.ko (6.6.118).
 * ELF offsets below refer to stock sections, not loaded virtual addresses.
 */
#include <linux/atomic.h>
#include <asm/barrier.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/swap.h>
#include <linux/sysfs.h>
#include <linux/tracepoint.h>
#include <linux/vmstat.h>
#include <trace/hooks/vmscan.h>

/* Private vmscan enum: exact values from the active mm/vmscan.c. */
enum scan_balance {
	SCAN_EQUAL,
	SCAN_FRACT,
	SCAN_ANON,
	SCAN_FILE,
};

#define GRECLAIM_ENABLED		BIT(1)
#define RECLAIM_RESCUE_ENABLED	BIT(2)
#define GRECLAIM_DEBUG		BIT(4)
#define RECLAIM_RESCUE_DEBUG	BIT(5)
#define RECLAIM_RESCUE_FLAG	BIT_ULL(0)
#define RECLAIM_OEM_SLOT		3
#define HIGH_PRIO		110

struct mi_memory_state {
	int low_free_enable;
	int greclaim_enable;
	int event;
	int madvise_interrupt;
	int reclaim_wmark;
	int interval_kill_time;
	int interval_reclaim_time;
	atomic_t wakeup_reclaim_count;
	atomic_t wakeup_slowpath_count;
	atomic_t wakeup_kill_count;
	atomic_t wakeup_throttle_count;
	atomic_t total_rescue_throttle_count;
	atomic_t rescue_app_throttle_count;
	atomic_t rescue_rt_throttle_count;
	atomic_t rescue_cfs_throttle_count;
	atomic_t rescue_other_throttle_count;
	atomic_t app_count;
	atomic_t rt_count;
	atomic_t cfs_high_pri_count;
	atomic_t other_count;
	unsigned long total_nr_reclaim;
	unsigned long total_reclaim_times;
	unsigned long file_down_threshold;
	unsigned long file_up_threshold;
	unsigned long anon_down_threshold;
	unsigned long anon_up_threshold;
	int scan_type;
	atomic_t epoll_event;
	struct mutex greclaim_lock;
	wait_queue_head_t epoll_event_wait;
	struct kobject *low_free_kobj;
	struct kobject *mi_reclaim_kobj;
};

static struct mi_memory_state memory;

static bool enabled(unsigned int flag)
{
	return READ_ONCE(memory.greclaim_enable) & flag;
}

static bool is_mi_reclaimer(void)
{
	return !strncmp(current->comm, "MiuiIntegratedM", 15);
}

static bool is_served_app(void)
{
	struct task_struct *leader = current->group_leader;
	const char *comm;

	if (!leader)
		return false;
	comm = leader->comm;
	return !strncmp(comm, "com.miui.home", 13) ||
		!strncmp(comm, "surfaceflinger", 14) ||
		!strncmp(comm, ".globallauncher", 15) ||
		!strncmp(comm, "system_server", 13);
}

static void account_task(atomic_t *app, atomic_t *rt, atomic_t *cfs,
			 atomic_t *other)
{
	if (is_served_app())
		atomic_inc(app);
	else if (current->prio < MAX_RT_PRIO)
		atomic_inc(rt);
	else if (current->prio <= HIGH_PRIO)
		atomic_inc(cfs);
	else
		atomic_inc(other);
}

/* .text+0xa08: kernel threads are excluded from scan-type tuning. */
static void tune_scan_type(void *unused, enum scan_balance *scan_type)
{
	int reclaim_type;

	if (current->flags & PF_KTHREAD)
		return;
	if (enabled(RECLAIM_RESCUE_ENABLED) &&
	    (READ_ONCE(current->android_oem_data1[RECLAIM_OEM_SLOT]) &
	     RECLAIM_RESCUE_FLAG)) {
		*scan_type = SCAN_FILE;
		if (enabled(RECLAIM_RESCUE_DEBUG)) {
			pr_info("%s adj %d pri %d high_pri %d scan_type %d",
				current->group_leader->comm,
				current->signal->oom_score_adj, current->prio,
				HIGH_PRIO, SCAN_FILE);
			account_task(&memory.app_count, &memory.rt_count,
				     &memory.cfs_high_pri_count,
				     &memory.other_count);
		}
	}
	reclaim_type = READ_ONCE(memory.scan_type);
	if (reclaim_type) {
		get_task_struct(current);
		if (is_mi_reclaimer() || enabled(GRECLAIM_DEBUG)) {
			*scan_type = READ_ONCE(memory.scan_type);
			if (enabled(GRECLAIM_DEBUG))
				pr_info("scan type %d", *scan_type);
		}
		put_task_struct(current);
	}
}

/* .text+0xc78: rescue is consumed by the next non-MI slab hook. */
static void shrink_slab_bypass(void *unused, gfp_t gfp_mask, int nid,
			       struct mem_cgroup *memcg, int priority,
			       bool *bypass)
{
	u64 flags;

	get_task_struct(current);
	if (is_mi_reclaimer()) {
		*bypass = true;
		goto out;
	}
	if (!enabled(RECLAIM_RESCUE_ENABLED))
		goto out;
	flags = READ_ONCE(current->android_oem_data1[RECLAIM_OEM_SLOT]);
	if (flags & RECLAIM_RESCUE_FLAG) {
		*bypass = true;
		flags &= ~RECLAIM_RESCUE_FLAG;
		WRITE_ONCE(current->android_oem_data1[RECLAIM_OEM_SLOT], flags);
	}
	if (enabled(RECLAIM_RESCUE_DEBUG))
		pr_info("slab bypass task flag %llu", flags);
out:
	put_task_struct(current);
}

/* .text+0xdc0: rescue applies to every task reaching this hook. */
static void throttle_direct_reclaim_bypass(void *unused, bool *bypass)
{
	u64 flags;

	if (!enabled(RECLAIM_RESCUE_ENABLED)) {
		pr_info("skip direct reclaim fuc not enabled");
		return;
	}
	flags = READ_ONCE(current->android_oem_data1[RECLAIM_OEM_SLOT]);
	WRITE_ONCE(current->android_oem_data1[RECLAIM_OEM_SLOT],
		   flags | RECLAIM_RESCUE_FLAG);
	if (enabled(RECLAIM_RESCUE_DEBUG)) {
		pr_info("skip %s throttle", current->group_leader->comm);
		account_task(&memory.rescue_app_throttle_count,
			     &memory.rescue_rt_throttle_count,
			     &memory.rescue_cfs_throttle_count,
			     &memory.rescue_other_throttle_count);
	}
	atomic_inc(&memory.total_rescue_throttle_count);
	*bypass = true;
}

static void log_memory(const char *when)
{
	unsigned long inactive_anon = global_node_page_state(NR_INACTIVE_ANON);
	unsigned long active_anon = global_node_page_state(NR_ACTIVE_ANON);
	unsigned long inactive_file = global_node_page_state(NR_INACTIVE_FILE);
	unsigned long active_file = global_node_page_state(NR_ACTIVE_FILE);

	pr_info("reclaim %s: free %lu cma_free %lu nr_anon_total %lu nr_file_total %lu nr_active_anon %lu nr_inactive_anon %lu nr_active_file %lu nr_inactive_file %lu",
		when, global_zone_page_state(NR_FREE_PAGES),
		global_zone_page_state(NR_FREE_CMA_PAGES),
		inactive_anon + active_anon, inactive_file + active_file,
		active_anon, inactive_anon, active_file, inactive_file);
}

/* .text+0x4: event is megabytes with scan overrides in its low two bits. */
static void do_reclaim(int event)
{
	unsigned long nr_anon, nr_file, nr_reclaimed = 0, reclaimed;
	unsigned long inactive_anon, active_anon, inactive_file, active_file;
	unsigned long free_pages, cma_free;
	unsigned long target_pages;
	ktime_t start;
	int scan_type, loops = 0, loop_limit, override = event & 3;

	if (!enabled(GRECLAIM_ENABLED)) {
		pr_info("global reclaim function not enabled");
		return;
	}
	mutex_lock(&memory.greclaim_lock);
	inactive_file = global_node_page_state(NR_INACTIVE_FILE);
	active_file = global_node_page_state(NR_ACTIVE_FILE);
	inactive_anon = global_node_page_state(NR_INACTIVE_ANON);
	active_anon = global_node_page_state(NR_ACTIVE_ANON);
	cma_free = global_zone_page_state(NR_FREE_CMA_PAGES);
	free_pages = global_zone_page_state(NR_FREE_PAGES);
	nr_anon = inactive_anon + active_anon;
	nr_file = inactive_file + active_file;
	if (nr_anon > memory.anon_up_threshold &&
	    nr_anon >= memory.anon_down_threshold)
		scan_type = SCAN_ANON;
	else if (nr_file > memory.file_up_threshold)
		scan_type = SCAN_FILE;
	else {
		if (nr_anon < memory.anon_down_threshold &&
		    enabled(GRECLAIM_DEBUG))
			pr_info("break reclaim: anon and file pages too lower nr_active_file_pages %lu, nr_inactive_file_pages %lu nr_active_anon_pages %lu, nr_inactive_anon_pages %lu anon_down_threshold %lu file_down_threshold %lu anon_up_threshold %lu file_up_threshold %lu",
				active_file, inactive_file, active_anon,
				inactive_anon,
				memory.anon_down_threshold,
				memory.file_down_threshold,
				memory.anon_up_threshold,
				memory.file_up_threshold);
		/* Stock clears scan_type after dropping the lock on this path. */
		mutex_unlock(&memory.greclaim_lock);
		WRITE_ONCE(memory.scan_type, 0);
		return;
	}
	if (override & 1)
		scan_type = SCAN_FILE;
	if (override & 2)
		scan_type = SCAN_ANON;
	WRITE_ONCE(memory.scan_type, scan_type);
	event = min(event, 1024);
	loop_limit = max(event / 10, 0);
	/* Negative stock events run zero iterations, avoiding a signed shift. */
	target_pages = event > 0 ? ((unsigned long)event << 20) >> PAGE_SHIFT : 0;
	if (enabled(GRECLAIM_DEBUG))
		pr_info("reclaim start: free %lu cma_free %lu nr_anon_total %lu nr_file_total %lu nr_active_anon %lu nr_inactive_anon %lu nr_active_file %lu nr_inactive_file %lu",
			free_pages, cma_free, nr_anon, nr_file, active_anon,
			inactive_anon, active_file, inactive_file);
	start = ktime_get();
	while (loops < loop_limit) {
		reclaimed = try_to_free_mem_cgroup_pages(NULL, 2560, GFP_KERNEL,
						      MEMCG_RECLAIM_MAY_SWAP);
		nr_reclaimed += reclaimed;
		loops++;
		if (reclaimed < 1280) {
			if (enabled(GRECLAIM_DEBUG))
				pr_info("break reclaim: inefficient want %dM, actual %lu pages, the last time %lu pages",
					event, nr_reclaimed, reclaimed);
			break;
		}
		if (nr_reclaimed > target_pages) {
			if (enabled(GRECLAIM_DEBUG))
				pr_info("break reclaim: the reclaim target has been achieved loops %d",
					loops);
			break;
		}
	}
	if (enabled(GRECLAIM_DEBUG))
		pr_info("reclaim info: time %lld ms loops %d event %d scan_type %d want_reclaim %d M actual_reclaim %lu pages",
			ktime_to_ms(ktime_sub(ktime_get(), start)), loops, override,
			scan_type, event, nr_reclaimed);
	WRITE_ONCE(memory.scan_type, 0);
	memory.total_nr_reclaim += nr_reclaimed;
	memory.total_reclaim_times++;
	mutex_unlock(&memory.greclaim_lock);
	if (enabled(GRECLAIM_DEBUG))
		log_memory("end");
}

/* Stock has no writer/wakeup for this counter; preserve that dormant ABI. */
static int lmk_event_show(struct seq_file *m, void *unused)
{
	int event = atomic_read(&memory.epoll_event);

	atomic_set(&memory.epoll_event, 0);
	seq_printf(m, "event %d\n", event);
	return 0;
}

static int lmk_event_open(struct inode *inode, struct file *file)
{
	return single_open(file, lmk_event_show, inode->i_private);
}

static __poll_t lmk_event_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &memory.epoll_event_wait, wait);
	return atomic_read(&memory.epoll_event) > 0 ? EPOLLIN : 0;
}

static const struct proc_ops event_proc_ops = {
	.proc_open = lmk_event_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_poll = lmk_event_poll,
};

#define INTEGER_ATTRIBUTE(_name, _field, _label) \
static ssize_t _name##_show(struct kobject *kobj, \
			   struct kobj_attribute *attr, char *buf) \
{ \
	return sysfs_emit(buf, _label " %d\n", READ_ONCE(memory._field)); \
} \
static ssize_t _name##_store(struct kobject *kobj, \
			    struct kobj_attribute *attr, const char *buf, \
			    size_t count) \
{ \
	int value, ret = kstrtoint(buf, 0, &value); \
	if (ret) \
		return ret; \
	WRITE_ONCE(memory._field, value); \
	return count; \
} \
static struct kobj_attribute kobj_attr_##_name = \
	__ATTR(_name, 0660, _name##_show, _name##_store)

INTEGER_ATTRIBUTE(low_free_enable, low_free_enable, "enable");
INTEGER_ATTRIBUTE(greclaim_enable, greclaim_enable, "enable");
INTEGER_ATTRIBUTE(madvise_interrupt, madvise_interrupt, "event");

static ssize_t low_free_parm_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"wakeup_reclaim_count %d\nwakeup_kill_count %d\nwakeup_throttle_count %d\nwakeup_slowpath_count %d\ninterval_kill_time %d\ninterval_reclaim_time %d\nreclaim_wmark %d\n",
		atomic_read(&memory.wakeup_reclaim_count),
		atomic_read(&memory.wakeup_kill_count),
		atomic_read(&memory.wakeup_throttle_count),
		atomic_read(&memory.wakeup_slowpath_count),
		READ_ONCE(memory.interval_kill_time),
		READ_ONCE(memory.interval_reclaim_time),
		READ_ONCE(memory.reclaim_wmark));
}

static ssize_t low_free_parm_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int wmark, kill_time, reclaim_time;

	if (sscanf(buf, "%d %d %d", &wmark, &kill_time, &reclaim_time) != 3) {
		pr_info("usage: total three args, wmark kill_time reclaim_time");
		return -EINVAL;
	}
	WRITE_ONCE(memory.reclaim_wmark, wmark);
	WRITE_ONCE(memory.interval_kill_time, kill_time);
	WRITE_ONCE(memory.interval_reclaim_time, reclaim_time);
	return count;
}

static ssize_t event_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "event %d\n", READ_ONCE(memory.event));
}

static ssize_t event_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	int value, ret = kstrtoint(buf, 0, &value);

	if (ret)
		return ret;
	do_reclaim(value);
	return count;
}

static ssize_t greclaim_parm_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	unsigned long file_down, file_up, anon_down, anon_up, total, times;

	mutex_lock(&memory.greclaim_lock);
	file_down = memory.file_down_threshold;
	file_up = memory.file_up_threshold;
	anon_down = memory.anon_down_threshold;
	anon_up = memory.anon_up_threshold;
	total = memory.total_nr_reclaim;
	times = memory.total_reclaim_times;
	mutex_unlock(&memory.greclaim_lock);
	return sysfs_emit(buf,
		"file_down_threshold %lu\nfile_up_threshold %lu\nanon_down_threshold %lu\nanon_up_threshold %lu\ntotal_nr_reclaim %lu\ntotal_reclaim_times %lu\napp_count %d\nrt_count %d\ncfs_high_pri_count %d\nother_count %d\ntotal_rescue_throttle_count %d\nrescue_rt_throttle_count %d\nrescue_cfs_throttle_count %d\nrescue_app_throttle_count %d\nrescue_other_throttle_count %d\n",
		file_down, file_up, anon_down, anon_up, total, times,
		atomic_read(&memory.app_count), atomic_read(&memory.rt_count),
		atomic_read(&memory.cfs_high_pri_count),
		atomic_read(&memory.other_count),
		atomic_read(&memory.total_rescue_throttle_count),
		atomic_read(&memory.rescue_rt_throttle_count),
		atomic_read(&memory.rescue_cfs_throttle_count),
		atomic_read(&memory.rescue_app_throttle_count),
		atomic_read(&memory.rescue_other_throttle_count));
}

static ssize_t greclaim_parm_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned long file_down, file_up, anon_down, anon_up;

	if (sscanf(buf, "%lu %lu %lu %lu", &file_down, &file_up,
		   &anon_down, &anon_up) != 4) {
		pr_info("usage: file down threshold, file up thresholdanon down threshold, anon up threshold");
		return -EINVAL;
	}
	mutex_lock(&memory.greclaim_lock);
	memory.file_down_threshold = file_down;
	memory.file_up_threshold = file_up;
	memory.anon_down_threshold = anon_down;
	memory.anon_up_threshold = anon_up;
	mutex_unlock(&memory.greclaim_lock);
	return count;
}

static struct kobj_attribute kobj_attr_low_free_parm =
	__ATTR(low_free_parm, 0660, low_free_parm_show, low_free_parm_store);
static struct kobj_attribute kobj_attr_event =
	__ATTR(event, 0660, event_show, event_store);
static struct kobj_attribute kobj_attr_greclaim_parm =
	__ATTR(greclaim_parm, 0660, greclaim_parm_show, greclaim_parm_store);

static struct attribute *low_free_attrs[] = {
	&kobj_attr_low_free_enable.attr, &kobj_attr_low_free_parm.attr, NULL,
};
static const struct attribute_group low_free_attr_group = {
	.attrs = low_free_attrs,
};
static struct attribute *mi_reclaim_attrs[] = {
	&kobj_attr_greclaim_enable.attr, &kobj_attr_event.attr,
	&kobj_attr_madvise_interrupt.attr, &kobj_attr_greclaim_parm.attr, NULL,
};
static const struct attribute_group mi_reclaim_attr_group = {
	.attrs = mi_reclaim_attrs,
};

static int __init mi_memory_init(void)
{
	struct kobject *kobj;
	int ret;

	memory.reclaim_wmark = 200;
	memory.interval_kill_time = 100;
	memory.interval_reclaim_time = 100;
	memory.file_down_threshold = 0x60000;
	memory.file_up_threshold = 0x80000;
	memory.anon_down_threshold = 0x60000;
	memory.anon_up_threshold = 0x6cccc;
	mutex_init(&memory.greclaim_lock);
	init_waitqueue_head(&memory.epoll_event_wait);
	if (!proc_create("lowmemorykiller", 0, NULL, &event_proc_ops))
		pr_err("error creating kernel lmk event file\n");
	dsb(sy);
	kobj = kobject_create_and_add("low_free", kernel_kobj);
	if (!kobj) {
		pr_err("failed to create low_free node.\n");
		goto fail_low_free;
	}
	ret = sysfs_create_group(kobj, &low_free_attr_group);
	if (ret) {
		pr_err("failed to create low_free attrs.\n");
		kobject_put(kobj);
		goto fail_low_free;
	}
	memory.low_free_kobj = kobj;
	kobj = kobject_create_and_add("mi_reclaim", kernel_kobj);
	if (!kobj) {
		pr_err("failed to create mi_reclaim node.\n");
		goto fail_mi_reclaim;
	}
	ret = sysfs_create_group(kobj, &mi_reclaim_attr_group);
	if (ret) {
		pr_err("failed to create mi_reclaim attrs.\n");
		kobject_put(kobj);
		goto fail_mi_reclaim;
	}
	memory.mi_reclaim_kobj = kobj;
	ret = register_trace_android_vh_tune_scan_type(tune_scan_type, NULL);
	if (ret) {
		pr_err("Failed to register tune_scan_type hook.\n");
		goto complete;
	}
	ret = register_trace_android_vh_shrink_slab_bypass(shrink_slab_bypass, NULL);
	if (ret) {
		pr_err("Failed to register shrink slab hook.\n");
		goto unregister_tune;
	}
	ret = register_trace_android_vh_throttle_direct_reclaim_bypass(
		throttle_direct_reclaim_bypass, NULL);
	if (ret) {
		pr_err("Failed to register throttle reclaim hook.\n");
		goto unregister_slab;
	}
complete:
	dsb(sy);
	WRITE_ONCE(memory.greclaim_enable, 7);
	pr_info("mi memory init ok\n");
	return 0;

unregister_slab:
	unregister_trace_android_vh_shrink_slab_bypass(shrink_slab_bypass, NULL);
unregister_tune:
	unregister_trace_android_vh_tune_scan_type(tune_scan_type, NULL);
	goto complete;
fail_mi_reclaim:
	memory.mi_reclaim_kobj = NULL;
	return -1;
fail_low_free:
	memory.low_free_kobj = NULL;
	return -1;
}

static void __exit mi_memory_exit(void)
{
	if (memory.low_free_kobj)
		kobject_put(memory.low_free_kobj);
	memory.low_free_kobj = NULL;
	if (memory.mi_reclaim_kobj)
		kobject_put(memory.mi_reclaim_kobj);
	memory.mi_reclaim_kobj = NULL;
	unregister_trace_android_vh_tune_scan_type(tune_scan_type, NULL);
	unregister_trace_android_vh_shrink_slab_bypass(shrink_slab_bypass, NULL);
	unregister_trace_android_vh_throttle_direct_reclaim_bypass(
		throttle_direct_reclaim_bypass, NULL);
}

module_init(mi_memory_init);
module_exit(mi_memory_exit);
MODULE_AUTHOR("zhangcang<zhangcang@xaiomi.com>");
MODULE_DESCRIPTION("memory");
MODULE_LICENSE("GPL");
