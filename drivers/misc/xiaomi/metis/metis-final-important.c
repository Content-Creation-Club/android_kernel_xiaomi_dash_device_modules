// SPDX-License-Identifier: GPL-2.0-only
/* Stock OS3.0.305 METIS .data+0x3eb0..0x3fc8; seven 0x28 records.
 * Strings are original relocation targets read from the retained program.
 * Native pointer relocations replace the old absolute Ghidra addresses. */
#include "metis_state.h"

union metis_important_task important_tasks[7] = {
	{ .fields = { .disabled = 1, .flags = 2 } },
	{ .fields = { .thread_pattern = "CRON.Main",
		.process_pattern = "tv.danmaku.bili", .flags = 1 } },
	{ .fields = { .thread_pattern = "DanmuDrawThread",
		.process_pattern = "com.sina.weibo", .flags = 1 } },
	{ .fields = { .thread_pattern = "KeyguardCameraV",
		.process_pattern = "ndroid.systemui", .flags = 1 } },
	{ .fields = { .thread_pattern = "*",
		.process_pattern = "com.miui.home", .flags = 16,
		.deadline_ms = 6, .main_thread_depend = 1 } },
	{ .fields = { .thread_pattern = "mali-mem-purge",
		.process_pattern = "surfaceflinger", .uid = 1000, .flags = 2 } },
	{ .fields = { .thread_pattern = "RenderEngine",
		.process_pattern = "surfaceflinger", .uid = 1000, .flags = 2 } },
};
