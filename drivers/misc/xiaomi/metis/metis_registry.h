/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_REGISTRY_H
#define _XIAOMI_METIS_REGISTRY_H
#include "metis_callbacks.h"
struct rq;
struct cpumask;
/* Recovered callback machines; native typedef/hash equivalence is untested. */
typedef void (*metis_fair_task_fn)(struct rq *, struct task_struct *, int flags);
typedef void (*metis_energy_fn)(struct task_struct *, bool *is_vip);
typedef bool (*metis_lb_fn)(struct task_struct *, int source,
			   int destination, int capacity_delta);
typedef bool (*metis_skip_cpus_fn)(struct cpumask *mask);
typedef void (*metis_walt_fn)(struct task_struct *, u32 *order,
			     int *end, int clusters, bool *selected);
extern metis_fair_task_fn mi_enqueue_task_fair_func[3];
extern metis_fair_task_fn mi_dequeue_task_fair_func[3];
extern metis_energy_fn wake_render_func[3];
extern metis_lb_fn mi_lb_detect_func[3];
extern metis_detect_fn choose_cpu_overutiled[3];
extern metis_skip_cpus_fn game_migt_skip_cpus_metis_hook;
extern metis_walt_fn mi_walt_get_indicies_func[3];

void register_mi_enqueue_task_fair_hook(unsigned int module, metis_fair_task_fn hook);
void unregister_mi_enqueue_task_fair_hook(unsigned int module);
void register_mi_dequeue_task_fair_hook(unsigned int module, metis_fair_task_fn hook);
void unregister_mi_dequeue_task_fair_hook(unsigned int module);
void register_mi_find_energy_efficient_cpu(unsigned int module, metis_energy_fn hook);
void unregister_mi_find_energy_efficient_cpu(unsigned int module);
void register_mi_walt_get_indicies(unsigned int module, metis_walt_fn hook);
void unregister_mi_walt_get_indicies(unsigned int module);
void register_mi_lb_detect_func(unsigned int module, metis_lb_fn hook);
void unregister_mi_lb_detect_func(unsigned int module);
void register_choose_cpu_overutiled(unsigned int module, metis_detect_fn hook);
void unregister_choose_cpu_overutiled(unsigned int module);
void register_game_migt_skip_cpus_metis_hook(metis_skip_cpus_fn hook);
void unregister_game_migt_skip_cpus_metis_hook(void);
bool mi_lb_enable(struct task_struct *task, int destination);
bool mi_sched_choose_cpu_overutiled(struct task_struct *task);
#endif
