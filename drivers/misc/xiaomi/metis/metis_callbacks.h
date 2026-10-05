/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_CALLBACKS_H
#define _XIAOMI_METIS_CALLBACKS_H
#include <linux/types.h>
struct task_struct;
/* Both task and pipeline consumers enforce stock KCFI 0x26f27c13. */
typedef bool (*metis_detect_fn)(struct task_struct *task);
struct metis_detect_slot {
	metis_detect_fn detect;
	metis_detect_fn pipeline;
};
/* Three stock slots. Owners/init/lifetime remain absent, with no defaults. */
extern struct metis_detect_slot render_task_detect[3];
extern struct metis_detect_slot ui_task_detect[3];
extern metis_detect_fn override_schedboost[3];
void register_render_task_detect(unsigned int module, metis_detect_fn detect,
				metis_detect_fn pipeline);
void unregister_render_task_detect(unsigned int module);
void register_ui_task_detect(unsigned int module, metis_detect_fn detect,
			    metis_detect_fn pipeline);
void unregister_ui_task_detect(unsigned int module);
void register_override_schedboost(unsigned int module, metis_detect_fn detect);
void unregister_override_schedboost(unsigned int module);
bool mi_render_task(struct task_struct *task);
bool mi_ui_task(struct task_struct *task);
bool mi_render_pipeline(struct task_struct *task);
bool mi_ui_pipeline(struct task_struct *task);
bool sched_boost_should_disable(struct task_struct *task);
#endif
