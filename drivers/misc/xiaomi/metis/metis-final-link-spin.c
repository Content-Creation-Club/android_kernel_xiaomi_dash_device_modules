// SPDX-License-Identifier: GPL-2.0-only
/* Stock METIS cold-start spin control. Native API/CFI/layout/runtime UNTESTED. */
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/osq_lock.h>
#include <linux/preempt.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <trace/hooks/dtask.h>
#include <trace/hooks/rwsem.h>
#include <trace/hooks/sched.h>
#include "../xr_qi/qi_provider.h"
#include "metis_blocked.h"
#include "metis_coldstart.h"
#include "metis_coldstart_policy.h"
#include "metis_common.h"
#include "metis-lowlt-trace.h"
#include "metis-final-link.h"
#include "metis-final-link-trace.h"

/* Existing metis_blocked.h records these as Xiaomi OEM payloads, never native
 * mutex/rwsem layouts. Validation quirks match stock and are not repaired. */
static struct metis_mutex_oem_unknown *spin_mutex_data(struct mutex *lock)
{
	return lock ? (void *)lock->android_oem_data1 : NULL;
}

static struct metis_rwsem_oem_unknown *spin_rwsem_data(struct rw_semaphore *sem)
{
	return sem ? (void *)sem->android_oem_data1 : NULL;
}

static bool spin_mutex_valid(struct metis_mutex_oem_unknown *data)
{
	if (!data || data->unknown_00 || data->deadline_08)
		return false;
	if (cs_debug)
		pr_info("metis-cs: %s: mutex oem data is zero\n", "__mi_mutex_data_valid");
	return true;
}

static bool spin_rwsem_valid(struct metis_rwsem_oem_unknown *data, u64 *word)
{
	if (!data)
		return false;
	*word = data->unknown_00;
	if (*word)
		return (*word >> 32) == 0xab9f19ab;
	if (data->deadline_08)
		return false;
	if (cs_debug)
		pr_info("metis-cs: %s: rwsem oem data is zero\n", "__mi_rwsem_data_valid");
	if (protect_cs) {
		data->unknown_00 = 0xab9f19ab00000000ULL;
		data->deadline_08 = 0;
		*word = data->unknown_00;
	} else {
		*word = data->unknown_00;
	}
	return true;
}

static void spin_rwsem_reset(struct metis_rwsem_oem_unknown *data)
{
	data->unknown_00 = 0xab9f19ab00000000ULL;
	data->deadline_08 = 0;
}

/* .text+0x1ae88, virtual 001238b0. The first write is 32 bits, but resetting
 * an empty wait_list writes all 64 bits to a different word value. */
void mutex_reset_data_no_waiter(struct mutex *lock)
{
	struct metis_mutex_oem_unknown *data = spin_mutex_data(lock);

	if (!spin_mutex_valid(data))
		return;
	if (protect_cs) {
		data->deadline_08 = 0;
		data->unknown_00 = 0x149f194f;
	}
	if (list_empty(&lock->wait_list)) {
		data->unknown_00 = 0;
		data->unknown_04 = 0x149f194f;
		data->deadline_08 = 0;
	}
}

/* .text+0x1bd24, virtual 0012474c; native dtask.h fixes unused probe data. */
void mi_mutex_can_spin_on_owner(void *unused, struct mutex *lock, int *ret)
{
	struct metis_mutex_oem_unknown *data;
	unsigned long deadline;

	if (!protect_cs || ((is_qi_feat_enabled(9) & 1) && is_qos_task(current)))
		return;
	if (!*ret || !lock || current->prio < 100)
		return;
	data = spin_mutex_data(lock);
	if (!spin_mutex_valid(data))
		return;
	if (protect_cs) {
		data->deadline_08 = 0;
		data->unknown_00 = 0x149f194f;
		deadline = 0;
	} else {
		if (!(data->unknown_00 & 0xffff))
			return;
		deadline = data->deadline_08;
	}
	if ((s64)(jiffies - deadline) < 0)
		*ret = 0;
}

/* .text+0x1c294, virtual 00124cbc. No lock NULL check exists in stock. */
void mi_mutex_init(void *unused, struct mutex *lock)
{
	struct metis_mutex_oem_unknown *data;

	if (!protect_cs)
		return;
	data = spin_mutex_data(lock);
	data->deadline_08 = 0;
	data->unknown_00 = 0x149f194f;
}

/* .text+0x1bbf0, virtual 00124618; native hook proves ignored lock/taken. */
void mi_mutex_opt_spin_finish(void *unused, struct mutex *lock, bool taken)
{
	struct mi_task_struct *state;

	if (!protect_cs || ((is_qi_feat_enabled(9) & 1) && is_qos_task(current)))
		return;
	state = get_mi_task_struct(current);
	if (!state || !MI_FIELD(state, u64, 0x2b8))
		return;
	/* Both the duration work and emission have separate original static
 * keys. The first predicate calls may expire and clear Xiaomi VIP state. */
	if (trace_lock_spin_dur_enabled()) {
		u64 elapsed = sched_clock();
		bool vip;

		elapsed -= READ_ONCE(MI_FIELD(state, u64, 0x2b8));
		vip = mi_vip_task(current) || mi_link_vip_task(current);

		trace_lock_spin_dur(current, taken, elapsed, "mutex", vip);
	}
	MI_FIELD(state, u64, 0x2b8) = 0;
}

/* .text+0x1ba84, virtual 001244ac; stock treats native signed count as wrapping
 * W arithmetic. Count zero stores clock and returns without incrementing it. */
void mi_mutex_opt_spin_start(void *unused, struct mutex *lock, bool *time_out,
		int *count)
{
	struct mi_task_struct *state;
	struct metis_mutex_oem_unknown *data;
	unsigned long deadline;
	u64 elapsed, start;
	s32 threshold;
	u32 next;

	if (!protect_cs || ((is_qi_feat_enabled(9) & 1) && is_qos_task(current)))
		return;
	state = get_mi_task_struct(current);
	if (!state)
		return;
	data = spin_mutex_data(lock);
	if (spin_mutex_valid(data)) {
		if (protect_cs) {
			data->deadline_08 = 0;
			data->unknown_00 = 0x149f194f;
			deadline = 0;
		} else {
			if (!(data->unknown_00 & 0xffff))
				goto count_spin;
			deadline = data->deadline_08;
		}
		if ((s64)(jiffies - deadline) < 0)
			goto timeout;
	}
count_spin:
	if (!*count) {
		MI_FIELD(state, u64, 0x2b8) = sched_clock();
		return;
	}
	next = (u32)*count + 1U;
	*count = (s32)next;
	if (next & 15U)
		return;
	start = MI_FIELD(state, u64, 0x2b8);
	if (!start)
		return;
	elapsed = sched_clock();
	elapsed -= READ_ONCE(MI_FIELD(state, u64, 0x2b8));
	threshold = mi_vip_task(current) || mi_link_vip_task(current) ?
		mutex_vip_spin_thres : mutex_normal_spin_thres;
	if (elapsed <= (u64)(s64)threshold)
		return;
timeout:
	*time_out = true;
}

/* .text+0x1c0b8, virtual 00124ae0. OEM validation/reset occurs before current
 * app/MM classification; tagged expiry resets even with concurrent disable. */
void mi_rwsem_can_spin_on_owner(void *unused, struct rw_semaphore *sem, bool *ret)
{
	struct metis_rwsem_oem_unknown *data;
	u64 word;
	s32 active = protect_cs;

	if (!active || !*ret || current->prio < 100)
		return;
	data = spin_rwsem_data(sem);
	if (!data) {
		active = 1;
	} else {
		word = data->unknown_00;
		if (!word) {
			if (data->deadline_08) {
				active = 1;
				goto app_check;
			}
			/* Unlike opt_spin_start, the raw can-spin body retains its
 * entry enable value unless its debug printk forces a fresh load. */
			if (cs_debug) {
				pr_info("metis-cs: %s: rwsem oem data is zero\n",
					"__mi_rwsem_data_valid");
				active = protect_cs;
				if (!active) {
					word = data->unknown_00;
					goto flags_check;
				}
			}
			spin_rwsem_reset(data);
			goto app_check;
		}
		if ((word >> 32) != 0xab9f19ab) {
			active = 1;
			goto app_check;
		}
flags_check:
		if (word & 0xffff) {
			if ((s64)(jiffies - data->deadline_08) < 0)
				goto no_spin;
			if ((word >> 32) == 0xab9f19ab)
				spin_rwsem_reset(data);
		}
	}
app_check:
	if (current->tgid != cold_start_app_tgid || !current->tgid || !active)
		return;
	if ((s64)(coldstart_end_jif - coldstart_begin_jif) >= 0 ||
	    (s64)(jiffies - coldstart_begin_jif - 1000UL) >= 0) {
		cold_start_sserver_tgid = 0;
		cold_start_app_tgid = 0;
		home_tgid = 0;
		systemui_tgid = 0;
		systemserver_tgid = 0;
		sf_tgid = 0;
		cold_start_app_mmlock = NULL;
		cold_start_sserver_mmlock = NULL;
		home_mmlock = NULL;
		systemui_mmlock = NULL;
		systemserver_mmlock = NULL;
		sf_mmlock = NULL;
		return;
	}
	if (!current->mm || &current->mm->mmap_lock != sem ||
	    mi_vip_task(current) || mi_link_vip_task(current))
		return;
no_spin:
	*ret = false;
}

/* .text+0x1c2d8, virtual 00124d00, literal RET. Native rwsem.h proves unused
 * callback signature; this original no-op is not a replacement producer. */
void mi_rwsem_direct_rsteal(void *unused, struct rw_semaphore *sem, bool *steal)
{
}

/* .text+0x1c2b8, virtual 00124ce0. */
void mi_rwsem_init(void *unused, struct rw_semaphore *sem)
{
	if (protect_cs)
		spin_rwsem_reset(spin_rwsem_data(sem));
}

/* .text+0x1bf9c, virtual 001249c4; native dtask.h proves lock/taken formals. */
void mi_rwsem_opt_spin_finish(void *unused, struct rw_semaphore *sem, bool taken)
{
	struct mi_task_struct *state;

	if (!protect_cs)
		return;
	state = get_mi_task_struct(current);
	if (!state || !MI_FIELD(state, u64, 0x2b8))
		return;
	if (trace_lock_spin_dur_enabled()) {
		u64 elapsed = sched_clock();
		bool vip;

		elapsed -= READ_ONCE(MI_FIELD(state, u64, 0x2b8));
		vip = mi_vip_task(current) || mi_link_vip_task(current);

		trace_lock_spin_dur(current, taken, elapsed, "rwsem", vip);
	}
	MI_FIELD(state, u64, 0x2b8) = 0;
}

/* .text+0x1be10, virtual 00124838. */
void mi_rwsem_opt_spin_start(void *unused, struct rw_semaphore *sem,
		bool *time_out, int *count, bool check_only)
{
	struct mi_task_struct *state;
	struct metis_rwsem_oem_unknown *data;
	u64 word, start, elapsed;
	u32 next;
	s32 threshold;

	if (!protect_cs)
		return;
	state = get_mi_task_struct(current);
	if (!state)
		return;
	data = spin_rwsem_data(sem);
	if (spin_rwsem_valid(data, &word) && (word & 0xffff)) {
		if ((s64)(jiffies - data->deadline_08) < 0)
			goto timeout;
		if ((word >> 32) == 0xab9f19ab)
			spin_rwsem_reset(data);
	}
	if (!*count) {
		if (!check_only)
			MI_FIELD(state, u64, 0x2b8) = sched_clock();
		return;
	}
	next = (u32)*count + 1U;
	*count = (s32)next;
	if (next & 15U)
		return;
	start = MI_FIELD(state, u64, 0x2b8);
	if (!start)
		return;
	elapsed = sched_clock();
	elapsed -= READ_ONCE(MI_FIELD(state, u64, 0x2b8));
	threshold = mi_vip_task(current) || mi_link_vip_task(current) ?
		rwsem_vip_spin_thres : rwsem_normal_spin_thres;
	if (elapsed <= (u64)(s64)threshold)
		return;
timeout:
	*time_out = true;
}

/* .text+0x1dd70, virtual 00126798. Values 1/2/4/8 are observed owner-state
 * results; original local C enum spelling is UNKNOWN. No private enum copied.
 * Original has no RCU section around owner->on_cpu. */
u32 rwsem_spin_on_owner(struct rw_semaphore *sem)
{
	unsigned long initial = atomic_long_read(&sem->owner), next;
	struct task_struct *owner, *new_owner;

	if (initial & 2)
		return 8;
	if (initial & 1)
		return 4;
	owner = (void *)(initial & ~3UL);
	if (!owner)
		return 1;
	next = atomic_long_read(&sem->owner);
	new_owner = (void *)(next & ~3UL);
	while (new_owner == owner && !((next ^ initial) & 3)) {
		if (need_resched() || !READ_ONCE(owner->on_cpu))
			return 8;
		/* Original also reads task->__state without using the result. */
		(void)READ_ONCE(owner->__state);
		cpu_relax();
		next = atomic_long_read(&sem->owner);
		new_owner = (void *)(next & ~3UL);
	}
	if (next & 2)
		return 8;
	if (next & 1)
		return 4;
	return new_owner ? 2 : 1;
}

/* .text+0x1c2e0, virtual 00124d08. Canonical sem count/owner/osq members;
 * native kernel/locking/rwsem.c defines READER_BIAS=1<<8 and writer/handoff
 * bits 0/2. The unused adjustment formal is proven by native rwsem hook.
 * No max spin count, no reference/RCU stabilization, as in stock. */
void mi_rwsem_optimistic_rspin(void *unused, struct rw_semaphore *sem,
		long *adjustment, bool *rspin)
{
	u32 state, previous = 1, next;
	unsigned long old;
	bool taken = false;

	if (!(protect_cs & 4) || *rspin || is_qos_task(current) ||
	    current->tgid != cold_start_app_tgid)
		return;
	trace_tracing_mark_write('B', current->tgid, "rwsem rspin", 0, 0);
	atomic_long_add(-256, &sem->count);
	preempt_disable();
	if (osq_lock(&sem->osq)) {
		state = rwsem_spin_on_owner(sem);
		while (state & 7) {
			next = state;
			if (!(atomic_long_read(&sem->count) & 5)) {
				old = atomic_long_fetch_add_acquire(256, &sem->count);
				if (!(old & 5)) {
					atomic_long_set(&sem->owner, (unsigned long)current |
						(atomic_long_read(&sem->owner) & 2) | 1);
					taken = true;
					break;
				}
				atomic_long_add(-256, &sem->count);
			}
			if (next != 2 && (need_resched() ||
			    (previous != 2 && current->prio < 100)))
				break;
			cpu_relax();
			state = rwsem_spin_on_owner(sem);
			previous = next;
		}
		osq_unlock(&sem->osq);
	}
	preempt_enable();
	*rspin = taken;
	if (!taken)
		atomic_long_add(256, &sem->count);
	/* Two separate event intervals exist in the stock jump-table tails. */
	trace_tracing_mark_write('E', current->tgid, "", 0, 0);
	trace_tracing_mark_write('B', current->tgid, "rspin with lock get", 0, *rspin);
	trace_tracing_mark_write('E', current->tgid, "", 0, 0);
}

/* .text+0x1b930, virtual 00124358; typed native wrappers preserve call order
 * and ignored errors. No cast of a mismatched callback is introduced. */
void register_cs_hooks(void)
{
	register_trace_android_vh_mutex_opt_spin_start(mi_mutex_opt_spin_start, NULL);
	register_trace_android_vh_mutex_opt_spin_finish(mi_mutex_opt_spin_finish, NULL);
	register_trace_android_vh_mutex_can_spin_on_owner(mi_mutex_can_spin_on_owner, NULL);
	register_trace_android_vh_rwsem_opt_spin_start(mi_rwsem_opt_spin_start, NULL);
	register_trace_android_vh_rwsem_opt_spin_finish(mi_rwsem_opt_spin_finish, NULL);
	register_trace_android_vh_rwsem_can_spin_on_owner(mi_rwsem_can_spin_on_owner, NULL);
	register_trace_android_vh_mutex_init(mi_mutex_init, NULL);
	register_trace_android_vh_rwsem_init(mi_rwsem_init, NULL);
	register_trace_android_vh_rwsem_direct_rsteal(mi_rwsem_direct_rsteal, NULL);
	register_trace_android_vh_rwsem_optimistic_rspin(mi_rwsem_optimistic_rspin, NULL);
	register_trace_android_vh_record_rwsem_writer_owned(mi_record_rwsem_writer_owned_hook, NULL);
	register_trace_android_vh_clear_rwsem_writer_owned(mi_clear_rwsem_writer_owned_hook, NULL);
	register_trace_android_vh_record_rwsem_reader_owned(mi_record_rwsem_reader_owned_hook, NULL);
}

/* .text+0x1c8ec, virtual 00125314. No tracepoint_synchronize_unregister. */
void unregister_cs_hooks(void)
{
	unregister_trace_android_vh_mutex_opt_spin_start(mi_mutex_opt_spin_start, NULL);
	unregister_trace_android_vh_mutex_opt_spin_finish(mi_mutex_opt_spin_finish, NULL);
	unregister_trace_android_vh_mutex_can_spin_on_owner(mi_mutex_can_spin_on_owner, NULL);
	unregister_trace_android_vh_rwsem_opt_spin_start(mi_rwsem_opt_spin_start, NULL);
	unregister_trace_android_vh_rwsem_opt_spin_finish(mi_rwsem_opt_spin_finish, NULL);
	unregister_trace_android_vh_rwsem_can_spin_on_owner(mi_rwsem_can_spin_on_owner, NULL);
	unregister_trace_android_vh_mutex_init(mi_mutex_init, NULL);
	unregister_trace_android_vh_rwsem_init(mi_rwsem_init, NULL);
	unregister_trace_android_vh_rwsem_direct_rsteal(mi_rwsem_direct_rsteal, NULL);
	unregister_trace_android_vh_rwsem_optimistic_rspin(mi_rwsem_optimistic_rspin, NULL);
	unregister_trace_android_vh_record_rwsem_writer_owned(mi_record_rwsem_writer_owned_hook, NULL);
	unregister_trace_android_vh_clear_rwsem_writer_owned(mi_clear_rwsem_writer_owned_hook, NULL);
	unregister_trace_android_vh_record_rwsem_reader_owned(mi_record_rwsem_reader_owned_hook, NULL);
}

/* .text+0x1dd0c, virtual 00126734. */
int metis_cs_init(void)
{
	register_cs_hooks();
	register_trace_android_rvh_sched_balance_rt(metis_coldstart_sched_balance_rt, NULL);
	pr_info("metis-cs: cs init!!\n");
	return 0;
}

/* .text+0x1dd54, virtual 0012677c. Cold-start RT hook is not unregistered. */
void metis_cs_exit(void)
{
	unregister_cs_hooks();
}

/* .text+0x1dbc4, virtual 001265ec. */
void register_coldstart_hooks(void)
{
	register_trace_android_rvh_sched_balance_rt(metis_coldstart_sched_balance_rt, NULL);
}

/* .text+0x1dd04, virtual 0012672c. Literal original RET, true void(void). */
void unregister_coldstart_hooks(void)
{
}
