/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XIAOMI_METIS_STATE_H
#define _XIAOMI_METIS_STATE_H
#include <linux/types.h>
#include "../mi_schedule/mi_schedule.h"

struct task_struct;
typedef bool (*metis_vip_detect_fn)(struct task_struct *task);
/* Stock .bss vip_task_detect is 0x30 bytes, callbacks at 0,16,32. */
struct metis_vip_detect_slot {
	metis_vip_detect_fn detect;
	u64 unknown_08;
};

/* Actual state owners, initialization, control interfaces and lifetime have
 * not yet been reconstructed. No substitute defaults or init are provided. */
extern struct metis_vip_detect_slot vip_task_detect[3];
extern unsigned int metis_debug;
extern unsigned int foreground_uid;
extern u8 fas_ktime_suspended;
extern s32 fas_ktime_last;
/* Stock .data+0x3eb0: seven private records, each 0x28 bytes. Typed
 * pointer fields retain native relocations; raw access preserves consumers. */
union metis_important_task {
	u8 raw[0x28];
	struct {
		const char *thread_pattern;
		const char *process_pattern;
		u32 disabled;
		u32 uid;
		u32 flags;
		s32 deadline_ms;
		u32 main_thread_depend;
		u32 unknown_24;
	} fields;
};
extern union metis_important_task important_tasks[7];

unsigned int cpu_to_cluster(unsigned int cpu);
u64 fas_sched_clock(void);
unsigned int get_foreground_uid(void);
void set_foreground_app(unsigned int uid);
bool mi_vip_task(struct task_struct *task);
void mi_set_fas_req(struct mi_task_struct *state);
void mi_clean_fas_req(struct mi_task_struct *state);
void register_vip_task_detect(unsigned int module, metis_vip_detect_fn detect);
void unregister_vip_task_detect(unsigned int module);
bool is_static_vip(struct mi_task_struct *state);
bool is_top_app_vip(struct mi_task_struct *state);
void clear_task_low_lt(struct mi_task_struct *state);
const char *mi_link_mask_to_type(long mask);
#endif
