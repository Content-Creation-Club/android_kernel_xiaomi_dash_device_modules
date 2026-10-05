// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reconstructed from dash OS3.0.305 xm_power.ko (6.6.118).
 * Preserve the stock behavior, including its incomplete failure/unload paths.
 * Evidence and known defects: RECONSTRUCTION.txt in this directory.
 *
 * The arithmetic routines follow the stock copies of Linux lib/math/div64.c;
 * CPU-time adjustment follows the stock copy of kernel/sched/cputime.c.
 */

#include <linux/device.h>
#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/pm_wakeup.h>
#include <linux/rtc.h>
#include <linux/rtmutex.h>
#include <linux/sched/cputime.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#define PID_HASH_BITS	10
#define TOP_TASK_COUNT	5

struct pid_entry {
	pid_t pid;
	char comm[TASK_COMM_LEN];
	u64 utime;
	u64 stime;
	struct hlist_node node;
};

struct task_info {
	pid_t pid;
	pid_t ppid;
	char comm[TASK_COMM_LEN];
	char pcomm[TASK_COMM_LEN];
	u64 utime_ms;
	u64 stime_ms;
	u64 total_ms;
	unsigned long cpus_mask;
};

static DEFINE_HASHTABLE(hash_table, PID_HASH_BITS);
/* The reference's .bss pid_lock is zero-initialized, with no init call. */
static struct rt_mutex pid_lock;
static struct delayed_work power_info_dump;
static struct kobject *xm_power_kobj;
static ktime_t PrevKtime;
static bool reset_write_flag;
static u32 wakelock_print_period_s = 10;
static u32 task_print_period_s = 20;

/* AArch64 UDIV by zero returns zero, including in the stock logger. */
static u64 stock_udiv(u64 dividend, u64 divisor)
{
	return divisor ? div64_u64(dividend, divisor) : 0;
}

static u64 div64_u64_bak(u64 dividend, u64 divisor)
{
	u32 high = divisor >> 32;
	u64 quot;
	int n;

	if (!high)
		return stock_udiv(dividend, (u32)divisor);

	n = fls(high);
	quot = stock_udiv(dividend >> n, (u32)(divisor >> n));
	if (quot)
		quot--;
	if (dividend - quot * divisor >= divisor)
		quot++;
	return quot;
}

static u64 div64_u64_rem_bak(u64 dividend, u64 divisor, u64 *remainder)
{
	u32 high = divisor >> 32;
	u64 quot;
	int n;

	if (!high) {
		divisor = (u32)divisor;
		quot = stock_udiv(dividend, divisor);
		*remainder = dividend - quot * divisor;
		return quot;
	}
	n = fls(high);
	quot = stock_udiv(dividend >> n, (u32)(divisor >> n));
	if (quot)
		quot--;
	*remainder = dividend - quot * divisor;
	if (*remainder >= divisor) {
		quot++;
		*remainder -= divisor;
	}
	return quot;
}

static u64 mul_u64_u64_div_u64_bak(u64 a, u64 b, u64 c)
{
	u64 res = 0, div, rem;
	int shift;

	if (ilog2(a) + ilog2(b) > 62) {
		div = div64_u64_rem_bak(b, c, &rem);
		res = div * a;
		b = rem;
		shift = ilog2(a) + ilog2(b) - 62;
		if (shift > 0) {
			b >>= shift;
			c >>= shift;
			if (!c)
				return res;
		}
	}
	return res + div64_u64_bak(a * b, c);
}

static void get_current_timestamp(const char *name)
{
	struct timespec64 ts;
	struct rtc_time tm;

	ktime_get_real_ts64(&ts);
	rtc_time64_to_tm(ts.tv_sec - sys_tz.tz_minuteswest * 60, &tm);
	/* Stock divides nanoseconds by 1000 despite the minimum width of 3. */
	pr_info("PowerDet:%s:%02d-%02d-%02d %02d:%02d:%02d.%03d(android time)\n",
		name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		(int)(ts.tv_nsec / NSEC_PER_USEC));
}

static void xm_cputime_adjust(struct task_cputime *curr,
			      struct prev_cputime *prev, u64 *ut, u64 *st)
{
	u64 rtime, stime, utime;
	unsigned long flags;

	raw_spin_lock_irqsave(&prev->lock, flags);
	rtime = curr->sum_exec_runtime;
	if (prev->stime + prev->utime >= rtime)
		goto out;
	stime = curr->stime;
	utime = curr->utime;
	if (!stime) {
		utime = rtime;
		goto update;
	}
	if (!utime) {
		stime = rtime;
		goto update;
	}
	stime = mul_u64_u64_div_u64_bak(stime, rtime, stime + utime);
update:
	/* Stock omits the current kernel's stime > rtime clamp. */
	if (stime < prev->stime)
		stime = prev->stime;
	utime = rtime - stime;
	if (utime < prev->utime) {
		utime = prev->utime;
		stime = rtime - utime;
	}
	prev->stime = stime;
	prev->utime = utime;
out:
	*ut = prev->utime;
	*st = prev->stime;
	raw_spin_unlock_irqrestore(&prev->lock, flags);
}

static void xm_task_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	struct task_cputime curr = {
		.sum_exec_runtime = p->se.sum_exec_runtime,
		.utime = p->utime,
		.stime = p->stime,
	};

	xm_cputime_adjust(&curr, &p->prev_cputime, ut, st);
}

static struct pid_entry *find_pid_entry(pid_t pid, struct task_struct *task,
				      struct pid_entry *previous)
{
	struct pid_entry *entry;

	if (previous && previous->pid == pid)
		return previous;
	hash_for_each_possible(hash_table, entry, node, pid) {
		if (entry->pid == pid)
			return entry;
	}
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return NULL;
	entry->pid = pid;
	strcpy(entry->comm, task->comm);
	hash_add(hash_table, &entry->node, pid);
	return entry;
}

static void rank_task(struct task_info *top)
{
	struct task_info tmp;
	int i, j;

	/* .text+0xbc8..0xec4: compare slots from 5 down to i+1. */
	for (i = 0; i < TOP_TASK_COUNT; i++) {
		for (j = TOP_TASK_COUNT; j > i; j--) {
			if (top[j].total_ms > top[i].total_ms) {
				tmp = top[i];
				top[i] = top[j];
				top[j] = tmp;
			}
		}
	}
}

static void dump_wakeup_sources(void)
{
	struct wakeup_source *ws, *last = NULL, *last_sec = NULL;
	bool active_seen = false;
	int srcuidx;

	srcuidx = wakeup_sources_read_lock();
	for_each_wakeup_source(ws) {
		if (ws->active) {
			pr_info("PowerDet:active wake lock: %s, active_since: %lld ms , active_count: %lu\n",
				ws->name, ktime_to_ms(ktime_sub(ktime_get(), ws->last_time)),
				ws->active_count);
			active_seen = true;
		} else if (!active_seen && (!last || ws->last_time > last->last_time)) {
			last_sec = last;
			last = ws;
		}
	}
	if (!active_seen && last)
		pr_info("PowerDet:last active wakeup source: %s, last_time:%lld ms, active_count: %lu\n",
			last->name, ktime_to_ms(last->last_time), last->active_count);
	if (!active_seen && last_sec)
		pr_info("PowerDet:last sec active wakeup source: %s, last_time:%lld ms, active_count: %lu\n",
			last_sec->name, ktime_to_ms(last_sec->last_time), last_sec->active_count);
	wakeup_sources_read_unlock(srcuidx);
}

static void dump_cpu_info(void)
{
	static ktime_t last;
	struct task_info top[TOP_TASK_COUNT + 1] = { };
	struct task_struct *group, *task;
	struct pid_entry *entry = NULL;
	ktime_t now;
	u64 utime, stime, elapsed_ms;
	int i;

	if ((u64)ktime_to_ms(ktime_sub(ktime_get(), last)) <
	    (u32)(task_print_period_s * 1000))
		return;
	last = ktime_get();
	rt_mutex_lock(&pid_lock);
	now = ktime_get();
	rcu_read_lock();
	if (!PrevKtime)
		goto done;

	/* Preserve the reference's circular thread_group traversal. */
	for_each_process(group) {
		task = group;
		do {
			entry = find_pid_entry(task->pid, task, entry);
			if (!entry) {
				rcu_read_unlock();
				rt_mutex_unlock(&pid_lock);
				pr_err("PowerDet:%s: failed to find the pid_entry for pid %d\n",
				       "print_all_task_stack", task->pid);
				return;
			}
			if (task->flags & PF_EXITING)
				continue;
			xm_task_cputime_adjusted(task, &utime, &stime);
			if (utime >= entry->utime && stime >= entry->stime &&
			    (utime > entry->utime || stime > entry->stime)) {
				top[TOP_TASK_COUNT].pid = task->pid;
				/* Stock ppid/pcomm are the thread group's leader. */
				top[TOP_TASK_COUNT].ppid = task->group_leader->pid;
				strcpy(top[TOP_TASK_COUNT].comm, task->comm);
				strcpy(top[TOP_TASK_COUNT].pcomm, task->group_leader->comm);
				top[TOP_TASK_COUNT].utime_ms =
					ktime_to_ms((ktime_t)(utime - entry->utime));
				top[TOP_TASK_COUNT].stime_ms =
					ktime_to_ms((ktime_t)(stime - entry->stime));
				top[TOP_TASK_COUNT].total_ms = top[TOP_TASK_COUNT].utime_ms +
							    top[TOP_TASK_COUNT].stime_ms;
				top[TOP_TASK_COUNT].cpus_mask = task->cpus_mask.bits[0];
				rank_task(top);
			}
			entry->utime = utime;
			entry->stime = stime;
			strcpy(entry->comm, task->comm);
		} while_each_thread(group, task);
	}
	elapsed_ms = ktime_to_ms(ktime_sub(now, PrevKtime));
	get_current_timestamp("dump_top5_task");
	for (i = 0; i < TOP_TASK_COUNT; i++)
		pr_info("PowerDet:Cpu usage:%d%% user + %d%% kernel, comm:%s, pid:%d, pcomm:%s, ppid:%d\n",
			(int)stock_udiv(top[i].utime_ms * 100, elapsed_ms),
			(int)stock_udiv(top[i].stime_ms * 100, elapsed_ms),
			top[i].comm, top[i].pid, top[i].pcomm, top[i].ppid);
done:
	rcu_read_unlock();
	PrevKtime = ktime_get();
	rt_mutex_unlock(&pid_lock);
}

static void power_info_dump_func(struct work_struct *work)
{
	pr_info("PowerDet:start \n");
	dump_wakeup_sources();
	dump_cpu_info();
	schedule_delayed_work(&power_info_dump,
		round_jiffies_relative(msecs_to_jiffies(wakelock_print_period_s * 1000)));
}

static ssize_t reset_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sprintf(buf, "%d\n", reset_write_flag);
}

static ssize_t reset_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	const char *newline = memchr(buf, '\n', count);
	size_t len = newline ? newline - buf : count;

	if (len == 5 && !strncmp(buf, "reset", 5)) {
		wakelock_print_period_s = 10;
		task_print_period_s = 20;
		reset_write_flag = true;
	} else {
		reset_write_flag = false;
	}
	return count;
}

static ssize_t task_print_period_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", (int)task_print_period_s);
}

static ssize_t task_print_period_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned long long value;

	if (kstrtoull(buf, 10, &value))
		return -EINVAL;
	task_print_period_s = (u32)value;
	return count;
}

static ssize_t wakelock_print_period_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", (int)wakelock_print_period_s);
}

static ssize_t wakelock_print_period_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	unsigned long long value;

	if (kstrtoull(buf, 10, &value))
		return -EINVAL;
	wakelock_print_period_s = (u32)value;
	return count;
}

static struct kobj_attribute reset_attr = __ATTR(reset, 0644, reset_show, reset_store);
static struct kobj_attribute task_print_period_attr =
	__ATTR(task_print_period, 0644, task_print_period_show, task_print_period_store);
static struct kobj_attribute wakelock_print_period_attr =
	__ATTR(wakelock_print_period, 0644, wakelock_print_period_show, wakelock_print_period_store);

static struct attribute *xm_power_attrs[] = {
	&reset_attr.attr,
	&task_print_period_attr.attr,
	&wakelock_print_period_attr.attr,
	NULL,
};

static const struct attribute_group xm_power_group = {
	.attrs = xm_power_attrs,
};

static const struct attribute_group *xm_power_groups[] = {
	&xm_power_group,
	NULL,
};

static int __init powerdet_init(void)
{
	pr_info("PowerDet:start \n");
	hash_init(hash_table);
	INIT_DELAYED_WORK(&power_info_dump, power_info_dump_func);
	/* Stock queues work before creating sysfs, without error unwind. */
	schedule_delayed_work(&power_info_dump,
		round_jiffies_relative(msecs_to_jiffies(wakelock_print_period_s * 1000)));
	xm_power_kobj = kobject_create_and_add("xm_power", NULL);
	if (!xm_power_kobj)
		return -ENOMEM;
	return sysfs_create_groups(xm_power_kobj, xm_power_groups);
}

static void __exit powerdet_exit(void)
{
	pr_info("PowerDet:powerdet_exit\n");
	/* Deliberately preserve the stock non-synchronous, partial teardown. */
	cancel_delayed_work(&power_info_dump);
}

module_init(powerdet_init);
module_exit(powerdet_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XiaoMI inc.");
MODULE_DESCRIPTION("The Power detector");
