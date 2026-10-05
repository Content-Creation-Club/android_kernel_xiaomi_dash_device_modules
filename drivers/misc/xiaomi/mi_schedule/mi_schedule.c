// SPDX-License-Identifier: GPL-2.0
/* dash OS3.0.305 stock mi_schedule reconstruction. Evidence and deliberately
 * retained defects are recorded in RECONSTRUCTION.txt. */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/bitmap.h>
#include <linux/cred.h>
#include <linux/sched/user.h>
#include <linux/sched/signal.h>
#include <linux/workqueue.h>
#include <linux/topology.h>
#include <linux/ktime.h>
#include <linux/interrupt.h>
#include <asm/unaligned.h>
#include <trace/hooks/sched.h>
#include "kernel/sched/sched.h"
#include "mi_schedule.h"

#define UID_LIMIT 100000
#define STOCK_NR_CPUS 32
static int develop_mode = 1;
static int runtime_traced_uid = 1000;
static int compact_traced_uid = 1000;
static int traced_window = 3;
static int runtime_window_size = 30;
int pause_mode;
static int curr_runtime_items, uid_max_value;
static int top_package_uid[4];
static int cpu_cluster_map[STOCK_NR_CPUS + 1];
static bool runtime_enable, mi_task_has_init;
static DECLARE_BITMAP(package_uid_32B, UID_LIMIT);
static DECLARE_BITMAP(package_uid_bit, UID_LIMIT);
static struct kmem_cache *mi_task_struct_cachep, *mi_user_struct_cachep;
static struct workqueue_struct *sched_wq;
static struct work_struct mi_user_monitor_wk;
static u64 package_runtime_times[6];
static u64 *package_runtime_info;
static unsigned long sys_update_time = 0xffffffffUL;
static struct mi_user_struct zero_mi_user;
static struct user_struct zero_user = {
	.__count = REFCOUNT_INIT(1),
	.android_oem_data1 = { MI_USER_OEM_MAGIC, (unsigned long)&zero_mi_user },
};
static struct user_struct *user_zero;
static mi_sched_load_update_fn SCHED_LOAD_UPDATE_HOOK[3];
DEFINE_PER_CPU_SHARED_ALIGNED(struct mi_rq, mi_runqueues);
EXPORT_SYMBOL_GPL(mi_runqueues);

/* .text+0x1a9c..0x1b6c */
struct mi_user_struct *get_mi_user_struct(struct user_struct *u)
{
	if (u->android_oem_data1[0] == MI_USER_OEM_MAGIC)
		return (void *)u->android_oem_data1[1];
	return NULL;
}
EXPORT_SYMBOL_GPL(get_mi_user_struct);

bool mi_task_exiting(struct task_struct *p)
{
	return !p || p->exit_state || READ_ONCE(p->__state) == TASK_DEAD;
}
EXPORT_SYMBOL_GPL(mi_task_exiting);

struct mi_task_struct *get_mi_task_struct_unsafe(struct task_struct *p)
{
	if (!p || p->android_oem_data1[0] != MI_TASK_OEM_MAGIC)
		return NULL;
	return (void *)p->android_oem_data1[1];
}
EXPORT_SYMBOL_GPL(get_mi_task_struct_unsafe);

struct mi_task_struct *get_mi_task_struct(struct task_struct *p)
{
	if (mi_task_exiting(p))
		return NULL;
	return get_mi_task_struct_unsafe(p);
}
EXPORT_SYMBOL_GPL(get_mi_task_struct);

/* .text+0x1830..0x1a98: cached cluster order follows the first online CPU. */
static void mi_cpu_coregroup_mask(unsigned int cpu, struct cpumask *mask)
{
	int i;
	BUG_ON(cpu > STOCK_NR_CPUS);
	if (cpu_topology[cpu].cluster_id == -1)
		return;
	for_each_possible_cpu(i) {
		if (i >= STOCK_NR_CPUS)
			break;
		if (cpu_topology[i].cluster_id == cpu_topology[cpu].cluster_id)
			cpumask_set_cpu(i, mask);
	}
}

int get_cluster_type(unsigned int cpu)
{
	struct cpumask mask, online;
	int i, j, cluster = 0;
	if (!cpu_cluster_map[STOCK_NR_CPUS]) {
		for_each_online_cpu(i) {
			if (i >= STOCK_NR_CPUS)
				break;
			cpumask_clear(&mask);
			mi_cpu_coregroup_mask(i, &mask);
			cpumask_and(&online, &mask, cpu_online_mask);
			if (cpumask_first(&online) == i) {
				for_each_cpu(j, &mask)
					if (j < STOCK_NR_CPUS)
						cpu_cluster_map[j] = cluster;
				cluster++;
			}
		}
		if (!cpu_cluster_map[STOCK_NR_CPUS])
			cpu_cluster_map[STOCK_NR_CPUS] = 1;
	}
	BUG_ON(cpu > STOCK_NR_CPUS);
	return cpu_cluster_map[cpu] > 2 ? 0 : cpu_cluster_map[cpu];
}
EXPORT_SYMBOL_GPL(get_cluster_type);

u32 is_32B_user(int uid)
{
	if (uid > 99999)
		return 1;
	/* Stock deliberately has no lower-bound check. */
	return test_bit(uid, package_uid_32B);
}
EXPORT_SYMBOL_GPL(is_32B_user);

/* Only the bytes written by stock are initialized: .text+0x241c..0x2580,
 * .text+0x2ec0..0x3208. A blanket memset would change metis/migt state. */
static void init_mi_task(struct mi_task_struct *t, struct task_struct *p)
{
	static const u16 zero64[] = {
		0x230, 0x238, 0x240, 0x248, 0x250,
		0x1b0, 0x1b8, 0x1c0, 0x1d8, 0x1e0, 0x1e8, 0x1f0,
		0x1f8, 0x200, 0x278, 0x280, 0x288, 0x290,
		0x2b0, 0x2b8, 0x2d0, 0x2d8, 0x328,
	};
	int i;
	t->magic = MI_STATE_MAGIC;
	t->guard = 0xdcbadcbadcbadcbaULL;
	t->user_linked = 0;
	t->work_pending = 0;
	t->unknown_ba = 0;
	put_unaligned(0xffffffffULL, (u64 *)(t->raw + 0xbc));
	t->task = p;
	t->user = NULL;
	t->initial_exec_runtime = p->se.sum_exec_runtime;
	memset(t->raw + 0x110, 0, 0x90);
	MI_FIELD(t, u32, 0x1a0) = 0;
	MI_FIELD(t, u32, 0x1c8) = 0;
	MI_FIELD(t, u32, 0x208) = 0;
	MI_FIELD(t, u32, 0x298) = 0xffffffff;
	t->unknown_c8 = 0;
	t->rb_magic_270 = 0;
	RB_CLEAR_NODE(&t->rb_node_258);
	INIT_LIST_HEAD(&t->node_2c0);
	MI_FIELD(t, u64, 0x2f8) = ~0ULL;
	for (i = 0; i < ARRAY_SIZE(zero64); i++)
		MI_FIELD(t, u64, zero64[i]) = 0;
}

void mi_task_fork(void *unused, struct task_struct *p)
{
	struct mi_task_struct *t;
	if (!p)
		return;
	p->android_oem_data1[1] = 0;
	p->android_oem_data1[0] = 0;
	t = in_interrupt() ? NULL : kmem_cache_alloc(mi_task_struct_cachep, GFP_KERNEL);
	if (!t) {
		pr_err("mi-sched: tsk %s %d alloc mi_task failed\n", p->comm, p->pid);
		return;
	}
	init_mi_task(t, p);
	p->android_oem_data1[1] = (unsigned long)t;
	p->android_oem_data1[0] = MI_TASK_OEM_MAGIC;
}
EXPORT_SYMBOL(mi_task_fork);

void free_mi_task_struct(void *unused, struct task_struct *p)
{
	struct mi_task_struct *t;
	struct mi_rq *rq;
	struct rb_root_cached *root;
	unsigned long flags;
	rwlock_t *lock;
	if (p->android_oem_data1[0] != MI_TASK_OEM_MAGIC)
		return;
	t = (void *)p->android_oem_data1[1];
	p->android_oem_data1[0] = 0;
	if (t->magic != MI_STATE_MAGIC)
		return;
	BUG_ON(task_cpu(p) >= STOCK_NR_CPUS);
	rq = per_cpu_ptr(&mi_runqueues, task_cpu(p));
	t->magic = 0;
	if (t->user_linked && t->user) {
		lock = &t->user->lock;
		write_lock_irq(lock);
		t->user = NULL;
		if (t->user_linked)
			list_del(&t->user_node);
		write_unlock_irq(lock);
	}
	if (t->rb_magic_270 == 0xbadc) {
		t->rb_magic_270 = 0;
		if (!RB_EMPTY_NODE(&t->rb_node_258)) {
			root = &MI_FIELD(rq, struct rb_root_cached, 0x160);
			if (root->rb_leftmost == &t->rb_node_258)
				root->rb_leftmost = rb_next(&t->rb_node_258);
			rb_erase(&t->rb_node_258, &root->rb_root);
			RB_CLEAR_NODE(&t->rb_node_258);
		}
	}
	spin_lock_irqsave(&MI_FIELD(rq, spinlock_t, 0x1c8), flags);
	if (!list_empty(&t->node_2c0))
		list_del_init(&t->node_2c0);
	spin_unlock_irqrestore(&MI_FIELD(rq, spinlock_t, 0x1c8), flags);
	kmem_cache_free(mi_task_struct_cachep, t);
}
EXPORT_SYMBOL(free_mi_task_struct);

static struct user_struct *task_user(struct task_struct *p)
{
	struct user_struct *u;
	rcu_read_lock();
	rcu_read_lock();
	u = __task_cred(p)->user;
	rcu_read_unlock();
	if (!u)
		u = &zero_user;
	else
		get_uid(u);
	rcu_read_unlock();
	return u;
}

void register_sched_load_update_hook(int mod, mi_sched_load_update_fn fn)
{
	if (mod < 3) {
		pr_info("mi-sched: mod %d %s now\n", mod, __func__);
		BUG_ON((unsigned int)mod > 2);
		SCHED_LOAD_UPDATE_HOOK[mod] = fn;
	} else {
		pr_err("mi-sched: invalid mod register\n");
	}
}
EXPORT_SYMBOL(register_sched_load_update_hook);
void unregister_sched_load_update_hook(int mod)
{
	if (mod < 3) {
		pr_info("mi-sched: mod %d %s now\n", mod, __func__);
		BUG_ON((unsigned int)mod > 2);
		SCHED_LOAD_UPDATE_HOOK[mod] = NULL;
	} else {
		pr_err("mi-sched: invalid mod unregister\n");
	}
}
EXPORT_SYMBOL(unregister_sched_load_update_hook);
bool pkg_enable(void) { return pause_mode == 0; }
EXPORT_SYMBOL_GPL(pkg_enable);

/* .text+0x1bb0..0x1f40: callbacks run even when accounting is paused. */
void update_pkg_load(struct task_struct *p, int cpu, int event,
		u64 wallclock, u64 delta_exec)
{
	struct user_struct *u;
	struct mi_user_struct *mu;
	struct mi_task_struct *t;
	int cluster = get_cluster_type(cpu), slot = curr_runtime_items % 4, uid, i;
	if (!runtime_enable || pause_mode || (p->flags & PF_IDLE))
		goto callbacks;
	u = task_user(p);
	if (!u)
		u = &zero_user;
	uid = __kuid_val(u->uid);
	if (test_tsk_thread_flag(p, TIF_32BIT) && uid < UID_LIMIT)
		set_bit(uid, package_uid_32B);
	mu = get_mi_user_struct(u);
	if (mu) {
		t = get_mi_task_struct(p);
		if (t) {
			switch (cluster) {
			case 0: t->little_runtime[4] += delta_exec;
				mu->little_runtime[4] += delta_exec; break;
			case 1: t->big_runtime[4] += delta_exec;
				mu->big_runtime[4] += delta_exec; break;
			case 2: t->sup_runtime[4] += delta_exec;
				mu->sup_runtime[4] += delta_exec; break;
			}
			BUG_ON((unsigned int)slot > 5);
			t->sup_runtime[slot] = t->sup_runtime[4];
			t->big_runtime[slot] = t->big_runtime[4];
			t->little_runtime[slot] = t->little_runtime[4];
		}
	} else {
		if (uid > 99998)
			uid = 99999;
		if (!test_and_set_bit(uid, package_uid_bit)) {
			get_uid(u);
			if (uid_max_value < uid)
				uid_max_value = uid;
		}
	}
	if (u != &zero_user)
		free_uid(u);
callbacks:
	for (i = 0; i < 3; i++)
		if (SCHED_LOAD_UPDATE_HOOK[i])
			SCHED_LOAD_UPDATE_HOOK[i](p, cpu, event, wallclock, delta_exec);
}
EXPORT_SYMBOL_GPL(update_pkg_load);

struct mi_task_work {
	struct work_struct work;               /* 0x00, stock size 0x30 */
	int action;                            /* 0x30 */
	int pad;
	struct mi_task_struct *task;           /* 0x38 */
	struct mi_user_struct *user;           /* 0x40 */
	u32 uid;                               /* 0x48 */
	u32 tail;
};

/* .text+0x2800..0x2a90: stock async actions add / move / remove UID links. */
static void task_add_work(struct work_struct *work)
{
	struct mi_task_work *w = container_of(work, struct mi_task_work, work);
	struct mi_task_struct *t = w->task;
	struct mi_user_struct *u = w->user;
	rwlock_t *lock;
	if (w->action == 2) {
		if (t->user) {
			lock = &t->user->lock;
			write_lock_irq(lock);
			if (t->work_pending && t->user_linked) {
				list_del(&t->user_node);
				t->user_linked = 0;
				t->work_pending = 0;
			}
			t->user = NULL;
			t->uid = ~0U;
			write_unlock_irq(lock);
		}
		t->user = NULL;
		t->uid = ~0U;
		goto out;
	}
	if (w->action == 1 && t->user) {
		lock = &t->user->lock;
		write_lock_irq(lock);
		if (t->user_linked) {
			list_del(&t->user_node);
			t->user_linked = 0;
		}
		t->user = NULL;
		t->uid = ~0U;
		write_unlock_irq(lock);
	}
	if (w->action != 0 && w->action != 1)
		goto out;
	write_lock_irq(&u->lock);
	if (t->work_pending && !t->user_linked) {
		list_add(&t->user_node, &u->tasks);
		t->user_linked = 1;
		t->work_pending = 0;
	}
	t->user = u;
	t->uid = w->uid;
	write_unlock_irq(&u->lock);
out:
	t->work_pending = 0;
	put_task_struct(t->task);
	free_uid(u->user);
	kfree(w);
}

/* .text+0x2040..0x2418 */
void mi_sched_tick(void *unused, struct rq *rq)
{
	struct task_struct *p;
	struct user_struct *u;
	struct mi_user_struct *mu;
	struct mi_task_struct *t;
	struct mi_task_work *w;
	u64 now = get_jiffies_64();
	u32 uid;
	if (!runtime_enable || pause_mode)
		return;
	p = rq->curr;
	if (p->flags & PF_IDLE)
		goto roll;
	u = task_user(p);
	if (!u)
		goto roll;
	uid = __kuid_val(u->uid);
	get_task_struct(p);
	t = get_mi_task_struct(p);
	if (!t || !refcount_read(&p->usage))
		goto put;
	t->uid = uid;
	mu = get_mi_user_struct(u);
	if (mu && !t->user_linked && !t->work_pending && t->task) {
		w = kmalloc(sizeof(*w), GFP_ATOMIC);
		if (w) {
			t->work_pending = 1;
			get_task_struct(t->task);
			get_uid(mu->user);
			w->action = 0;
			w->task = t;
			w->user = mu;
			w->uid = uid;
			INIT_WORK(&w->work, task_add_work);
			queue_work(sched_wq, &w->work);
		}
	}
put:
	put_task_struct(p);
	if (u != &zero_user)
		free_uid(u);
roll:
	/* Keep stock's signed 32-bit multiplication followed by u64 comparison. */
	if ((u64)(s64)(runtime_window_size * 250) <= now - sys_update_time)
		queue_work_on(0, system_long_wq, &mi_user_monitor_wk);
}
EXPORT_SYMBOL(mi_sched_tick);

/* .text+0x2ec0..0x3208 */
static int mi_task_world_init(int *success)
{
	struct task_struct *g, *p;
	struct mi_task_struct *t;
	int failed = 0, count = 0;
	u32 uid;
	rcu_read_lock();
	for_each_process_thread(g, p) {
		get_task_struct(p);
		if (test_tsk_thread_flag(p, TIF_32BIT)) {
			uid = __kuid_val(task_uid(p));
			if (uid < UID_LIMIT)
				set_bit(uid, package_uid_32B);
		}
		if (p->android_oem_data1[0] == MI_TASK_OEM_MAGIC) {
			if (p->android_oem_data1[1]) {
				count++;
				goto put;
			}
		} else {
			t = in_interrupt() ? NULL : kmem_cache_alloc(mi_task_struct_cachep, GFP_ATOMIC);
			if (t) {
				init_mi_task(t, p);
				p->android_oem_data1[1] = (unsigned long)t;
				p->android_oem_data1[0] = MI_TASK_OEM_MAGIC;
				count++;
				goto put;
			}
		}
		pr_err("mi-sched: tsk %s %d alloc mi_task failed\n", p->comm, p->pid);
		failed++;
put:
		put_task_struct(p);
	}
	rcu_read_unlock();
	if (success)
		*success = count;
	return failed;
}

static void init_mi_user(struct mi_user_struct *mu, struct user_struct *u)
{
	mu->magic = MI_STATE_MAGIC;
	MI_FIELD(mu, u64, 0x10) = 0;
	INIT_LIST_HEAD(&mu->tasks);
	mu->initialized = 1;
	mu->user = u;
	get_uid(u);
	memset(mu->sup_runtime, 0, 0x90);
}

static void snapshot_user(struct mi_user_struct *mu, int slot)
{
	BUG_ON((unsigned int)slot > 5);
	mu->sup_runtime[slot] = mu->sup_runtime[4];
	mu->big_runtime[slot] = mu->big_runtime[4];
	mu->little_runtime[slot] = mu->little_runtime[4];
}

/* .text+0x2a94..0x2ebc */
static void mi_user_roll_wk(struct work_struct *work)
{
	struct user_struct *u;
	struct mi_user_struct *mu;
	struct mi_task_struct *t, *next;
	int uid = 0, max, success = 0, failed;
	s64 start;
	if (!mi_task_has_init) {
		mi_task_has_init = true;
		start = ktime_get_ns();
		failed = mi_task_world_init(&success);
		pr_info("mi-sched: Mi-task world init suc %d, fai %d tcost %llu\n",
			success, failed, (ktime_get_ns() - start) / 1000);
	}
	for (;;) {
		max = min(uid_max_value + 1, 99999);
		uid = find_next_bit(package_uid_bit, max, uid);
		max = min(uid_max_value + 1, 99999);
		if (uid >= max)
			break;
		u = find_user(KUIDT_INIT(uid));
		if (!u) {
			mu = get_mi_user_struct(&zero_user);
			snapshot_user(mu, curr_runtime_items);
			uid++;
			continue;
		}
		mu = get_mi_user_struct(u);
		if (u->android_oem_data1[0] != MI_USER_OEM_MAGIC) {
			mu = in_interrupt() ? NULL : kmem_cache_alloc(mi_user_struct_cachep, GFP_KERNEL);
			if (mu) {
				init_mi_user(mu, u);
				u->android_oem_data1[0] = MI_USER_OEM_MAGIC;
				u->android_oem_data1[1] = (unsigned long)mu;
			}
		}
		if (mu)
			snapshot_user(mu, curr_runtime_items);
		free_uid(u);
		/* Stock reads u after free_uid and relies on the retained bitmap ref. */
		if (refcount_read(&u->__count) == 1) {
			clear_bit(__kuid_val(u->uid), package_uid_bit);
			pr_info("mi-sched: User %d free mem\n", __kuid_val(u->uid));
			if (u->android_oem_data1[0] == MI_USER_OEM_MAGIC) {
				mu = (void *)u->android_oem_data1[1];
				u->android_oem_data1[0] = 0;
				if (mu->magic != MI_STATE_MAGIC)
					pr_err("mi-sched: Mi user struct has been destoryed\n");
				mu->magic = 0;
				write_lock_irq(&mu->lock);
				list_for_each_entry_safe(t, next, &mu->tasks, user_node) {
					list_del(&t->user_node);
					t->user = NULL;
					t->user_linked = 0;
				}
				write_unlock_irq(&mu->lock);
				kmem_cache_free(mi_user_struct_cachep, mu);
			}
			free_uid(u);
		}
		uid++;
	}
	BUG_ON((unsigned int)curr_runtime_items > 5);
	package_runtime_times[curr_runtime_items] = sys_update_time;
	curr_runtime_items = curr_runtime_items < 3 ? curr_runtime_items + 1 : curr_runtime_items - 3;
	sys_update_time = jiffies;
	package_runtime_times[4] = jiffies;
}

static int *get_curr_runtime_items(void) { return &curr_runtime_items; }
static int *get_uid_max_value(void) { return &uid_max_value; }
static int *get_runtime_window_size(void) { return &runtime_window_size; }
static struct user_struct *get_user_zero(void) { return &zero_user; }
static u64 *get_package_runtime_info(void) { return package_runtime_times; }

/* Proc clock policy uses stock HZ=250 literals and truncated usec conversion. */
static u64 runtime_delta(void)
{
	u32 delta = jiffies_to_usecs(jiffies - package_runtime_info[5]);
	package_runtime_info[4] = jiffies;
	if (develop_mode) {
		package_runtime_info[4] = jiffies;
		delta = jiffies_to_usecs(jiffies - package_runtime_info[5]);
	}
	return (u64)delta * 1000;
}
static void show_runtime_time(struct seq_file *m)
{
	seq_printf(m, "time info: from [%llu]s to [%llu]s ago\n",
		(u64)(jiffies - package_runtime_info[4]) / 250,
		(u64)(jiffies - package_runtime_info[5]) / 250);
}
static u32 percent(u64 runtime, u64 total)
{
	u32 value = total ? runtime * 10000 / total : 0;
	return (s32)value < 0 ? 0 : value;
}

/* .text+0x39c..0x8c8: UID references are released only on the printed path. */
static int runtime_show_all(struct seq_file *m, void *unused)
{
	struct user_struct *u;
	struct mi_user_struct *mu;
	u64 delta = runtime_delta(), sup, big, little, total;
	u64 sup_total = 0, big_total = 0, little_total = 0;
	u32 s, b, l, sbias, bbias, maxs = 0, maxb = 0, maxl = 0;
	int maxtotal = 0, total_s = 0, total_b = 0, total_l = 0;
	int uid = 0, max = min(*get_uid_max_value() + 1, 99999);
	if (!delta)
		return 0;
	show_runtime_time(m);
	for (;;) {
		uid = find_next_bit(package_uid_bit, max, uid);
		if (uid >= max)
			break;
		u = find_user(KUIDT_INIT(uid));
		if (!u)
			goto next;
		mu = get_mi_user_struct(u);
		if (!mu) {
			printk("mi user struct is null\n");
			goto next;
		}
		if (mu->sup_runtime[5] > mu->sup_runtime[4] ||
		    mu->big_runtime[5] > mu->big_runtime[4] ||
		    mu->little_runtime[5] > mu->little_runtime[4])
			goto next;
		sup = mu->sup_runtime[4] - mu->sup_runtime[5];
		big = mu->big_runtime[4] - mu->big_runtime[5];
		little = mu->little_runtime[4] - mu->little_runtime[5];
		total = sup + big + little;
		if (!total)
			goto next;
		/* Selection uses signed raw quotients, before display clamping. */
		s = sup * 10000 / delta;
		b = big * 10000 / delta;
		l = little * 10000 / delta;
		if ((s32)maxs <= (s32)s) { maxs = s; top_package_uid[1] = uid; }
		if ((s32)maxb <= (s32)b) { maxb = b; top_package_uid[2] = uid; }
		if ((s32)maxl <= (s32)l) { maxl = l; top_package_uid[3] = uid; }
		if (maxtotal < (int)(s + b + l)) {
			maxtotal = s + b + l;
			top_package_uid[0] = uid;
		}
		sbias = percent(sup, total);
		bbias = percent(big, total);
		sup_total += sup;
		big_total += big;
		little_total += little;
		total_s += (s32)s < 0 ? 0 : s;
		total_b += (s32)b < 0 ? 0 : b;
		total_l += (s32)l < 0 ? 0 : l;
		seq_printf(m, "%5d: <s>%16llu [%3d.%02d] <b>%16llu [%3d.%02d] <l>: %16llu ",
			uid, sup, (int)s / 100, (int)s % 100,
			big, (int)b / 100, (int)b % 100, little);
		/* Argument order follows stock, including its bias/display mismatch. */
		seq_printf(m, " [%3d.%02d]--bias: s-[%3d.%02d] b-[%3d.%02d] %s\n",
			sbias / 100, sbias % 100, bbias / 100, bbias % 100,
			(int)l / 100, (int)l % 100, is_32B_user(uid) ? "32BIT" : "64BIT");
		free_uid(u);
next:
		uid++;
	}
	seq_printf(m, "\nusage---sup cluster:[%d.%d] big cluster:[%d.%d] little cluster :[%d.%d]\n",
		total_s / 100, total_s % 100, total_b / 100, total_b % 100,
		total_l / 100, total_l % 100);
	seq_printf(m, "total_usage---sup cluster:[%llu] big cluster:[%llu] little cluster:[%llu] \n",
		sup_total, big_total, little_total);
	seq_printf(m, "top load package info--- 0 for s/b/l cluster\n");
	seq_printf(m, "%s: %d %3d.%02d\n", "g:     top_package", top_package_uid[0], maxtotal / 100, maxtotal % 100);
	seq_printf(m, "%s: %d %3d.%02d\n", "score: top_package", top_package_uid[1], (int)maxs / 100, (int)maxs % 100);
	seq_printf(m, "%s: %d %3d.%02d\n", "bcore: top_package", top_package_uid[2], (int)maxb / 100, (int)maxb % 100);
	seq_printf(m, "%s: %d %3d.%02d\n", "lcore: top_package", top_package_uid[3], (int)maxl / 100, (int)maxl % 100);
	return 0;
}

/* .text+0x91c..0xd1c */
static int package_runtime_show(struct seq_file *m, void *unused)
{
	struct task_struct *g, *p;
	struct mi_task_struct *t;
	int uid = *(int *)m->private;
	u64 delta = runtime_delta(), sup, big, little, total;
	u64 sup_total = 0, big_total = 0, little_total = 0;
	u32 s, b, l, sbias, bbias, compat = 0;
	int total_s = 0, total_b = 0, total_l = 0;
	seq_printf(m, "traced uid %d time delta is %llu\n", uid, delta);
	show_runtime_time(m);
	rcu_read_lock();
	for_each_process(g) {
		if (__kuid_val(task_uid(g)) != uid)
			continue;
		for_each_thread(g, p) {
			t = get_mi_task_struct(p);
			if (!t)
				continue;
			compat |= test_tsk_thread_flag(p, TIF_32BIT);
			if (t->sup_runtime[5] > t->sup_runtime[4] ||
			    t->big_runtime[5] > t->big_runtime[4] ||
			    t->little_runtime[5] > t->little_runtime[4])
				continue;
			sup = t->sup_runtime[4] - t->sup_runtime[5];
			big = t->big_runtime[4] - t->big_runtime[5];
			little = t->little_runtime[4] - t->little_runtime[5];
			total = sup + big + little;
			if (!total)
				continue;
			s = percent(sup, delta);
			b = percent(big, delta);
			l = percent(little, delta);
			sbias = percent(sup, total);
			bbias = percent(big, total);
			sup_total += sup;
			big_total += big;
			little_total += little;
			total_s += s;
			total_b += b;
			total_l += l;
			seq_printf(m, "%5d,%16s, s:%16llu [%3d.%02d], b:%16llu [%3d.%02d]",
				p->pid, p->comm, sup, s / 100, s % 100, big, b / 100, b % 100);
			seq_printf(m, "l:%16llu [%3d.%02d],| s:[%3d.%02d], b:[%3d.%02d] %s\n",
				little, l / 100, l % 100, sbias / 100, sbias % 100,
				bbias / 100, bbias % 100, test_tsk_thread_flag(p, TIF_32BIT) ? "32B" : "64B");
		}
	}
	seq_printf(m, "[%s]usage total sup:[%d.%d] big:[%d.%d] little:[%d.%d] \n",
		compat ? "32B" : "64B", total_s / 100, total_s % 100,
		total_b / 100, total_b % 100, total_l / 100, total_l % 100);
	seq_printf(m, "\ntotal_usage---sup cluster:[%llu] big cluster:[%llu] little cluster:[%llu] \n",
		sup_total, big_total, little_total);
	rcu_read_unlock();
	return 0;
}

/* .text+0x144c..0x16b4: reading the reset file changes every baseline. */
static int reset_traced_window_show(struct seq_file *m, void *unused)
{
	struct user_struct *u;
	struct mi_user_struct *mu;
	struct task_struct *g, *p;
	struct mi_task_struct *t;
	int slot = *get_curr_runtime_items();
	int max = min(*get_uid_max_value() + 1, 99999), uid = 0;
	slot = (u32)traced_window < 4 ? slot - traced_window + 4 : slot + 2;
	slot %= 4;
	if (develop_mode)
		slot = 4;
	for (;;) {
		uid = find_next_bit(package_uid_bit, max, uid);
		if (uid >= max)
			break;
		u = find_user(KUIDT_INIT(uid));
		if (u) {
			mu = get_mi_user_struct(u);
			if (!mu) {
				printk("mi user struct is null\n");
				uid++;
				continue;
			}
			BUG_ON((unsigned int)slot >= 6);
			mu->sup_runtime[5] = mu->sup_runtime[slot];
			mu->big_runtime[5] = mu->big_runtime[slot];
			mu->little_runtime[5] = mu->little_runtime[slot];
			free_uid(u);
		} else {
			mu = get_mi_user_struct(user_zero);
			BUG_ON((unsigned int)slot > 5);
			mu->sup_runtime[5] = mu->sup_runtime[slot];
			mu->big_runtime[5] = mu->big_runtime[slot];
			mu->little_runtime[5] = mu->little_runtime[slot];
		}
		/* Keep the stock NULL dereference after its zero-user fallback. */
		if (refcount_read(&u->__count) == 1) {
			clear_bit(__kuid_val(u->uid), package_uid_bit);
			free_uid(u);
		}
		uid++;
	}
	package_runtime_info[5] = jiffies;
	rcu_read_lock();
	for_each_process_thread(g, p) {
		t = get_mi_task_struct(p);
		if (!t)
			continue;
		BUG_ON((unsigned int)slot > 5);
		t->sup_runtime[5] = t->sup_runtime[slot];
		t->big_runtime[5] = t->big_runtime[slot];
		t->little_runtime[5] = t->little_runtime[slot];
	}
	rcu_read_unlock();
	seq_printf(m, "reset traced window show success!\n");
	return 0;
}

static int package_compact_show(struct seq_file *m, void *unused)
{
	int uid = *(int *)m->private;
	if (uid)
		seq_printf(m, "%d:%sBit\n", uid, is_32B_user(uid) ? "32" : "64");
	return 0;
}
static int package_trace_stat(struct seq_file *m, void *unused)
{
	seq_printf(m, "traced uid  is %d\n", runtime_traced_uid);
	seq_printf(m, "traced max uid is %d\n", *get_uid_max_value());
	return 0;
}
static int traced_window_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "traced window is last %d\n", traced_window);
	return 0;
}
static int window_size_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "window size is %d\n", *get_runtime_window_size());
	return 0;
}
static int develop_mode_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "develop mode %s\n", develop_mode ? "open" : "close");
	return 0;
}
static int pause_mode_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "pause mode %s\n", pause_mode ? "yes" : "no");
	return 0;
}

/* .text+0xe10..0xee0 / +0x117c..0x1240 / +0x131c..0x13cc /
 * +0x16e8..0x1798: 32-byte copy, no terminator at count >=32, return original
 * count even when copy_from_user fails. */
static ssize_t trace_package_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *pos)
{
	char text[32] = { 0 };
	int uid;
	if (!copy_from_user(text, buffer, min(count, sizeof(text)))) {
		uid = simple_strtol(text, NULL, 10);
		pr_err("got user info: %d....\n", uid);
		*(int *)((struct seq_file *)file->private_data)->private = uid;
	}
	return count;
}
static ssize_t traced_window_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *pos)
{
	char text[32] = { 0 };
	if (!copy_from_user(text, buffer, min(count, sizeof(text)))) {
		traced_window = simple_strtol(text, NULL, 10);
		pr_err("traced window %d\n", traced_window);
	}
	return count;
}
static ssize_t develop_mode_set(struct file *file, const char __user *buffer,
		size_t count, loff_t *pos)
{
	char text[32] = { 0 };
	if (!copy_from_user(text, buffer, min(count, sizeof(text))))
		develop_mode = simple_strtol(text, NULL, 10);
	return count;
}
static ssize_t pause_mode_set(struct file *file, const char __user *buffer,
		size_t count, loff_t *pos)
{
	char text[32] = { 0 };
	if (!copy_from_user(text, buffer, min(count, sizeof(text))))
		pause_mode = simple_strtol(text, NULL, 10);
	return count;
}

#define MI_OPEN(name, show) \
static int name(struct inode *inode, struct file *file) \
{ return single_open(file, show, NULL); }
MI_OPEN(runtime_open_all, runtime_show_all)
MI_OPEN(package_tracestat_open, package_trace_stat)
MI_OPEN(package_open_traced_window, traced_window_show)
MI_OPEN(package_open_window_size, window_size_show)
MI_OPEN(develop_mode_open, develop_mode_show)
MI_OPEN(package_reset_traced_window, reset_traced_window_show)
MI_OPEN(pause_mode_open, pause_mode_show)

#define MI_UID_OPEN(name, show, uidptr) \
static int name(struct inode *inode, struct file *file) \
{ \
	int ret = single_open(file, show, NULL); \
	if (!ret) \
		((struct seq_file *)file->private_data)->private = uidptr; \
	return ret; \
}
MI_UID_OPEN(top_package_on_bcore_open, package_runtime_show, &top_package_uid[2])
MI_UID_OPEN(top_package_on_lcore_open, package_runtime_show, &top_package_uid[3])
MI_UID_OPEN(top_package_open, package_runtime_show, &top_package_uid[0])
MI_UID_OPEN(package_runtime_open, package_runtime_show, &runtime_traced_uid)
MI_UID_OPEN(package_compact_open, package_compact_show, &compact_traced_uid)

#define MI_PROC_OPS(name, openfn, writefn) \
static const struct proc_ops name = { \
	.proc_open = openfn, .proc_read = seq_read, .proc_write = writefn, \
	.proc_lseek = seq_lseek, .proc_release = single_release, \
}
MI_PROC_OPS(package_show_all_fops, runtime_open_all, NULL);
MI_PROC_OPS(package_top_bcore_fops, top_package_on_bcore_open, NULL);
MI_PROC_OPS(package_top_lcore_fops, top_package_on_lcore_open, NULL);
MI_PROC_OPS(package_top_fops, top_package_open, NULL);
MI_PROC_OPS(package_trace_fops, package_runtime_open, trace_package_write);
MI_PROC_OPS(package_compact_fops, package_compact_open, trace_package_write);
MI_PROC_OPS(package_tracestat_fops, package_tracestat_open, NULL);
MI_PROC_OPS(package_tracedwindow_fops, package_open_traced_window, traced_window_write);
MI_PROC_OPS(package_windowsize_fops, package_open_window_size, NULL);
MI_PROC_OPS(develop_mode_fops, develop_mode_open, develop_mode_set);
MI_PROC_OPS(package_reset_tracedwindow_fops, package_reset_traced_window, NULL);
MI_PROC_OPS(pause_mode_fops, pause_mode_open, pause_mode_set);

static struct proc_dir_entry *package_rootdir, *package_dir, *stat_dir;
static struct proc_dir_entry *package_showall_entry, *package_top_bcore_entry;
static struct proc_dir_entry *package_top_lcore_entry, *package_top_entry;
static struct proc_dir_entry *package_trace_entry, *package_compact_entry;
static struct proc_dir_entry *package_tracestat_entry, *package_trace_window;
static struct proc_dir_entry *package_window_size_entry, *package_develop_mode_entry;
static struct proc_dir_entry *package_resettraced_window, *package_pause_mode_entry;

/* .text+0x4..0x248: per-file failures are ignored, no unwind. */
static void create_runtime_proc(void)
{
	pause_mode = 0;
	user_zero = get_user_zero();
	package_rootdir = proc_mkdir("package", NULL);
	if (!package_rootdir)
		return;
	package_dir = proc_mkdir("pkg", package_rootdir);
	if (!package_dir)
		return;
	package_showall_entry = proc_create("show_all", 0664, package_dir, &package_show_all_fops);
	package_top_bcore_entry = proc_create("top_package_on_bcore", 0664, package_dir, &package_top_bcore_fops);
	package_top_lcore_entry = proc_create("top_package_on_lcore", 0664, package_dir, &package_top_lcore_fops);
	package_top_entry = proc_create("top_package", 0664, package_dir, &package_top_fops);
	package_trace_entry = proc_create("show_traced_package", 0666, package_dir, &package_trace_fops);
	package_compact_entry = proc_create("show_32B_compact", 0666, package_dir, &package_compact_fops);
	stat_dir = proc_mkdir("stat", package_rootdir);
	if (!stat_dir)
		return;
	package_tracestat_entry = proc_create("show_tracestat", 0664, stat_dir, &package_tracestat_fops);
	package_trace_window = proc_create("show_traced_window", 0664, stat_dir, &package_tracedwindow_fops);
	package_window_size_entry = proc_create("show_windowsize", 0664, stat_dir, &package_windowsize_fops);
	package_develop_mode_entry = proc_create("develop_mode", 0666, stat_dir, &develop_mode_fops);
	package_resettraced_window = proc_create("reset_traced_window", 0664, stat_dir, &package_reset_tracedwindow_fops);
	package_pause_mode_entry = proc_create("pause_mode", 0666, stat_dir, &pause_mode_fops);
	package_runtime_info = get_package_runtime_info();
}

/* .text+0x24c..0x368: trace file removal comes after its directory removal. */
static void delete_runtimeproc(void)
{
	if (!package_rootdir)
		return;
	if (package_dir) {
		if (package_showall_entry) proc_remove(package_showall_entry);
		if (package_tracestat_entry) proc_remove(package_tracestat_entry);
		if (package_top_bcore_entry) proc_remove(package_top_bcore_entry);
		if (package_top_lcore_entry) proc_remove(package_top_lcore_entry);
		if (package_top_entry) proc_remove(package_top_entry);
		if (package_compact_entry) proc_remove(package_compact_entry);
		proc_remove(package_dir);
		if (!stat_dir)
			return;
		if (package_trace_entry) proc_remove(package_trace_entry);
		if (package_trace_window) proc_remove(package_trace_window);
		if (package_window_size_entry) proc_remove(package_window_size_entry);
		if (package_develop_mode_entry) proc_remove(package_develop_mode_entry);
		if (package_pause_mode_entry) proc_remove(package_pause_mode_entry);
		if (package_resettraced_window) proc_remove(package_resettraced_window);
		proc_remove(stat_dir);
	}
	proc_remove(package_rootdir);
}

/* .init.text+0x4..0x2cc / .exit.text+0x4..0x4c. Stock registers only
 * free-task/tick hooks; fork and runtime producers are external callers. */
static int __init mi_sched_init(void)
{
	struct mi_user_struct *mu;
	pr_err("in %s\n", __func__);
	mi_task_struct_cachep = kmem_cache_create("mi_task_struct", MI_TASK_STATE_SIZE, 0,
		SLAB_ACCOUNT | SLAB_PANIC | SLAB_HWCACHE_ALIGN, NULL);
	mi_user_struct_cachep = kmem_cache_create("mi_user_struct", 0xc0, 0,
		SLAB_ACCOUNT | SLAB_PANIC | SLAB_HWCACHE_ALIGN, NULL);
	sched_wq = alloc_workqueue("mi_sched", WQ_UNBOUND, 0);
	if (!sched_wq) {
		kmem_cache_destroy(mi_task_struct_cachep);
		kmem_cache_destroy(mi_user_struct_cachep);
		return 0;
	}
	create_runtime_proc();
	memset(package_runtime_times, 0, sizeof(package_runtime_times));
	mu = get_mi_user_struct(&zero_user);
	if (mu) {
		init_mi_user(mu, &zero_user);
		set_bit(0, package_uid_bit);
		if (uid_max_value < 0)
			uid_max_value = 0;
		get_uid(&zero_user);
	}
	INIT_WORK(&mi_user_monitor_wk, mi_user_roll_wk);
	sys_update_time = jiffies;
	package_runtime_times[4] = jiffies;
	package_runtime_times[5] = jiffies;
	register_trace_android_vh_free_task(free_mi_task_struct, NULL);
	register_trace_android_vh_scheduler_tick(mi_sched_tick, NULL);
	runtime_enable = true;
	return 0;
}
static void __exit mi_sched_exit(void)
{
	pr_err("in %s\n", __func__);
	delete_runtimeproc();
	kmem_cache_destroy(mi_task_struct_cachep);
	kmem_cache_destroy(mi_user_struct_cachep);
}
module_init(mi_sched_init);
module_exit(mi_sched_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("pkg runtime info calc by David");
