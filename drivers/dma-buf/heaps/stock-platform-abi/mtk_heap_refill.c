// SPDX-License-Identifier: GPL-2.0
/* Compile the existing refill algorithm against the stock provider's view. */
#include "stock-platform-abi.h"
#include "../mtk_heap_refill.c"

static_assert(sizeof(struct mtk_heap_priv_info) == 32);
static_assert(offsetof(struct mtk_heap_priv_info, page_pools) == 8);
static_assert(sizeof_field(struct mtk_heap_priv_info, page_pools) == 8);
static_assert(__same_type(typeof(&mtk_pool_below_refill_low),
			 bool (*)(struct mtk_dmabuf_page_pool *)));
static_assert(__same_type(typeof(&mtk_pool_above_recycle_high),
			 bool (*)(struct mtk_dmabuf_page_pool *)));
static_assert(__same_type(typeof(&refill_dma_heap_pool), int (*)(void *)));
static_assert(!IS_ENABLED(CONFIG_MTK_IOMMU_DEBUG),
		"The stock305 IOMMU debug callback ABI has not been audited");
