/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 GitHub @YorokobiMaster */
#ifndef _MIICMPFILTER_PARSER_H
#define _MIICMPFILTER_PARSER_H

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t __be32;
#endif

#define MIICMPFILTER_IPPROTO_ICMP	1
#define MIICMPFILTER_DEST_UNREACH	3
#define MIICMPFILTER_PORT_OFFSET	30
#define MIICMPFILTER_ICMP_BYTES		(MIICMPFILTER_PORT_OFFSET + 2)

struct miicmpfilter_event {
	u16 port;
	__be32 address;
};

static inline bool miicmpfilter_is_wlan(const char *name)
{
	/*
	 * Stock compares strlen(actual_name) bytes, accepting prefixes such as
	 * "wlan" as well as wlan0/wlan1. Preserve this selection ABI.
	 * Evidence: docs/driver-bringup/dash-miicmpfilter.md, MF004.
	 */
	return name && (!strncmp(name, "wlan0", strlen(name)) ||
			!strncmp(name, "wlan1", strlen(name)));
}

static inline bool miicmpfilter_parse_event(u8 protocol, __be32 source_address,
					    const u8 *icmp, size_t len,
					    struct miicmpfilter_event *event)
{
	if (!icmp || !event || protocol != MIICMPFILTER_IPPROTO_ICMP ||
	    len < MIICMPFILTER_ICMP_BYTES ||
	    icmp[0] != MIICMPFILTER_DEST_UNREACH)
		return false;

	event->port = ((u16)icmp[MIICMPFILTER_PORT_OFFSET] << 8) |
		      icmp[MIICMPFILTER_PORT_OFFSET + 1];
	event->address = source_address;
	return true;
}

#endif /* _MIICMPFILTER_PARSER_H */
