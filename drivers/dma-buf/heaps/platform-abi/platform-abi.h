/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HEAP_PLATFORM_ABI_H
#define HEAP_PLATFORM_ABI_H

#include <linux/build_bug.h>
#include <linux/mm.h>
#include <linux/stddef.h>

/* Suppress the native provider view only in this consumer translation unit. */
#define _MTK_DMABUF_PAGE_POOL_H
#define NUM_ORDERS 3

/*
 * OS3.0.305 system_heap Build ID: a8416e1f11d3ff8772ea788d000f714bf5653e15.
 * mtk_dmabuf_page_pool_create+0x28 allocates 0xa8 bytes. Only the fields
 * accessed by mtk_heap_refill are represented; provider-owned regions stay
 * opaque. In particular, bytes 0x50..0x5f have no inferred semantics.
 * Keep the structure tag: the provider's bool(pool *) KCFI ID is 0x34ad62bc.
 */
struct mtk_dmabuf_page_pool {
	u8 opaque_00_2b[0x2c];
	gfp_t gfp_mask;
	unsigned int order;
	u8 opaque_34_47[0x14];
	struct mtk_dmabuf_page_pool **pool_list;
	u8 opaque_50_5f[0x10];
	struct task_struct *refill_kthread;
	bool (*need_refill)(struct mtk_dmabuf_page_pool *pool);
	bool (*need_recycle)(struct mtk_dmabuf_page_pool *pool);
	int order_index;
	u8 opaque_7c_7f[4];
	bool refilling;
	bool recycling;
	u8 opaque_82_8f[0xe];
	u64 refill_time;
	u8 opaque_98_a7[0x10];
};

#define ASSERT_POOL_FIELD(member, offset, width) \
	static_assert(offsetof(struct mtk_dmabuf_page_pool, member) == offset); \
	static_assert(sizeof_field(struct mtk_dmabuf_page_pool, member) == width)

static_assert(PAGE_SHIFT == 12);
static_assert(sizeof(struct mtk_dmabuf_page_pool) == 0xa8);
ASSERT_POOL_FIELD(gfp_mask, 0x2c, 4);
ASSERT_POOL_FIELD(order, 0x30, 4);
ASSERT_POOL_FIELD(pool_list, 0x48, 8);
ASSERT_POOL_FIELD(refill_kthread, 0x60, 8);
ASSERT_POOL_FIELD(need_refill, 0x68, 8);
ASSERT_POOL_FIELD(need_recycle, 0x70, 8);
ASSERT_POOL_FIELD(order_index, 0x78, 4);
ASSERT_POOL_FIELD(refilling, 0x80, 1);
ASSERT_POOL_FIELD(recycling, 0x81, 1);
ASSERT_POOL_FIELD(refill_time, 0x90, 8);
#undef ASSERT_POOL_FIELD

int mtk_dmabuf_page_pool_total(struct mtk_dmabuf_page_pool *pool, bool high);
struct page *mtk_dmabuf_page_pool_fetch(struct mtk_dmabuf_page_pool *pool);
void mtk_dmabuf_page_pool_add(struct mtk_dmabuf_page_pool *pool, struct page *page);

#endif
