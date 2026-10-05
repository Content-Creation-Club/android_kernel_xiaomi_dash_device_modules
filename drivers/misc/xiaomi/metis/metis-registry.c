// SPDX-License-Identifier: GPL-2.0-only
/* PARTIAL: 16 observed registry/consumer bodies. Storage owners still absent. */
#include <linux/kernel.h>
#include <linux/sched.h>
#include "kernel/sched/sched.h"
#include "metis_registry.h"

enum registry_log { FUNCTION_ONLY, MODULE_FIRST, FUNCTION_FIRST };

/* A source helper for repeated stock checks, not a counted stock body. */
static bool registry_module(unsigned int module, const char *name,
			    const char *invalid, enum registry_log format)
{
	if ((int)module >= 3) {
		printk(KERN_ERR "metis-fas: invalid mod %s\n", invalid);
		return false;
	}
	if (format == MODULE_FIRST)
		pr_info("metis-fas: %d %s now\n", module, name);
	else if (format == FUNCTION_FIRST)
		pr_info("metis-fas: %s %d now\n", name, module);
	else
		pr_info("metis-fas: %s now\n", name);
	if (module >= 3) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	return true;
}

void register_mi_enqueue_task_fair_hook(unsigned int module, metis_fair_task_fn hook)
{
	if (registry_module(module, __func__, "register", FUNCTION_ONLY))
		mi_enqueue_task_fair_func[module] = hook;
}

void unregister_mi_enqueue_task_fair_hook(unsigned int module)
{
	/* Its invalid-unregister diagnostic really says register. */
	if (registry_module(module, __func__, "register", FUNCTION_ONLY))
		mi_enqueue_task_fair_func[module] = NULL;
}

void register_mi_dequeue_task_fair_hook(unsigned int module, metis_fair_task_fn hook)
{
	if (registry_module(module, __func__, "register", FUNCTION_ONLY))
		mi_dequeue_task_fair_func[module] = hook;
}

void unregister_mi_dequeue_task_fair_hook(unsigned int module)
{
	if (registry_module(module, __func__, "register", FUNCTION_ONLY))
		mi_dequeue_task_fair_func[module] = NULL;
}

void register_mi_find_energy_efficient_cpu(unsigned int module, metis_energy_fn hook)
{
	if (registry_module(module, __func__, "register", FUNCTION_ONLY))
		wake_render_func[module] = hook;
}

void unregister_mi_find_energy_efficient_cpu(unsigned int module)
{
	if (registry_module(module, __func__, "unregister", FUNCTION_ONLY))
		wake_render_func[module] = NULL;
}

void register_mi_walt_get_indicies(unsigned int module, metis_walt_fn hook)
{
	if (registry_module(module, __func__, "register", FUNCTION_ONLY))
		mi_walt_get_indicies_func[module] = hook;
}

void unregister_mi_walt_get_indicies(unsigned int module)
{
	if (registry_module(module, __func__, "unregister", FUNCTION_ONLY))
		mi_walt_get_indicies_func[module] = NULL;
}

void register_mi_lb_detect_func(unsigned int module, metis_lb_fn hook)
{
	if (registry_module(module, __func__, "register", FUNCTION_FIRST))
		mi_lb_detect_func[module] = hook;
}

void unregister_mi_lb_detect_func(unsigned int module)
{
	if (registry_module(module, __func__, "unregister", FUNCTION_FIRST))
		mi_lb_detect_func[module] = NULL;
}

void register_choose_cpu_overutiled(unsigned int module, metis_detect_fn hook)
{
	if (registry_module(module, __func__, "register", MODULE_FIRST))
		choose_cpu_overutiled[module] = hook;
}

void unregister_choose_cpu_overutiled(unsigned int module)
{
	if (registry_module(module, __func__, "register", MODULE_FIRST))
		choose_cpu_overutiled[module] = NULL;
}

void register_game_migt_skip_cpus_metis_hook(metis_skip_cpus_fn hook)
{
	if (hook)
		game_migt_skip_cpus_metis_hook = hook;
}

void unregister_game_migt_skip_cpus_metis_hook(void)
{
	game_migt_skip_cpus_metis_hook = NULL;
}

bool mi_lb_enable(struct task_struct *task, int destination)
{
	u32 source = task_cpu(task), source_capacity, destination_capacity;
	metis_lb_fn hook;

	if (source >= 32 || (u32)destination >= 32) {
		asm volatile("brk #0x5512");
		__builtin_unreachable();
	}
	hook = mi_lb_detect_func[0];
	/* Original LDR W consumes only the low32 bits of cpu_capacity_orig. */
	source_capacity = cpu_rq(source)->cpu_capacity_orig;
	destination_capacity = cpu_rq(destination)->cpu_capacity_orig;
	if (!hook)
		hook = mi_lb_detect_func[1];
	if (!hook)
		hook = mi_lb_detect_func[2];
	if (!hook)
		return false;
	return hook(task, source, destination,
		    (s32)(destination_capacity - source_capacity));
}

bool mi_sched_choose_cpu_overutiled(struct task_struct *task)
{
	bool result = false;
	unsigned int module;

	for (module = 0; module < 3; module++) {
		if (choose_cpu_overutiled[module])
			result |= choose_cpu_overutiled[module](task);
	}
	return result;
}
