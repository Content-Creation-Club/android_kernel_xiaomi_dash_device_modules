/* SPDX-License-Identifier: GPL-2.0 */
/* Recovered entry algorithms; module.c supplies source-only entry metadata. */
#ifndef XIAOMI_MIGT_MODULE_LIFECYCLE_H
#define XIAOMI_MIGT_MODULE_LIFECYCLE_H
#include "subsystem_lifecycle.h"
#include "../metis/metis_common.h"

/* The original .data+0x50 owner in boost.c is shared with its finish work. */
extern struct notifier_block init_freq_qos_notifier;

int migt_init(void);
void migt_exit(void);
#endif
