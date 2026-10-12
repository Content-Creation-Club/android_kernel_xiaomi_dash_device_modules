/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DASH_STOCK305_DISP_ABI_H
#define DASH_STOCK305_DISP_ABI_H

/*
 * OS3.0.305 mediatek-drm Build ID: c9dd9e330e02b9c67ab167d857331bc25962a565.
 * The stock and native consumers' instructions and normalized relocations
 * match. Assert only the fields they access, not the unused private objects.
 */
#define STOCK305_DISP_FIELD(type, member, offset, width) \
	static_assert(offsetof(type, member) == offset); \
	static_assert(sizeof_field(type, member) == width)

static_assert(PAGE_SHIFT == 12);
static_assert(IS_ENABLED(CONFIG_DEVICE_MODULES_ARM_SMMU_V3),
		"The non-SMMU secure GEM handle ABI has not been audited");
static_assert(IS_ENABLED(CONFIG_MTK_TRUSTED_MEMORY_SUBSYSTEM));
static_assert(sizeof(struct platform_driver) == 248);
static_assert(sizeof(struct mtk_drm_disp_mtee_cb) == 16);
static_assert(sizeof(struct mtk_drm_disp_sec_cb) == 8);
STOCK305_DISP_FIELD(struct mtk_drm_disp_mtee_cb, dev, 0, 8);
STOCK305_DISP_FIELD(struct mtk_drm_disp_mtee_cb, cb, 8, 8);
STOCK305_DISP_FIELD(struct mtk_ddp_comp, dev, 0x38, 8);
STOCK305_DISP_FIELD(struct mtk_ddp_comp, mtk_crtc, 0x40, 8);
STOCK305_DISP_FIELD(struct mtk_ddp_comp, id, 0x5c, 4);
STOCK305_DISP_FIELD(struct mtk_ddp_comp, fb, 0x68, 8);
STOCK305_DISP_FIELD(struct drm_device, dev_private, 0x38, 8);
STOCK305_DISP_FIELD(struct drm_device, mode_config.crtc_list, 0x370, 16);
STOCK305_DISP_FIELD(struct drm_crtc, dev, 0, 8);
STOCK305_DISP_FIELD(struct drm_crtc, head, 0x10, 16);
STOCK305_DISP_FIELD(struct drm_crtc, index, 0xa0, 4);
static_assert(offsetof(struct mtk_drm_crtc, base) == 0);
STOCK305_DISP_FIELD(struct mtk_drm_crtc, gce_obj.client[CLIENT_SEC_CFG], 0x8f8, 8);
STOCK305_DISP_FIELD(struct mtk_drm_crtc, gce_obj.base, 0x958, 8);
STOCK305_DISP_FIELD(struct mtk_drm_private, data, 0x2ad8, 8);
STOCK305_DISP_FIELD(struct mtk_drm_private, secure_static_path_switch, 0x319d, 1);
STOCK305_DISP_FIELD(struct mtk_mmsys_driver_data, mmsys_id, 0x40, 4);
STOCK305_DISP_FIELD(struct device, of_node, 0x300, 8);
static_assert(offsetof(struct platform_device, dev) == 0x10);
static_assert(sizeof(struct cmdq_cb_data) == 16);
STOCK305_DISP_FIELD(struct cmdq_cb_data, err, 0, 4);
STOCK305_DISP_FIELD(struct cmdq_cb_data, data, 8, 8);
static_assert(CLIENT_SEC_CFG == 6);
static_assert(DDP_COMPONENT_WDMA0 == 0x47 && DDP_COMPONENT_WDMA1 == 0x48);
static_assert(DISP_SEC_START == 0 && DISP_SEC_STOP == 1 && DISP_SEC_ENABLE == 2);
static_assert(DISP_SEC_DISABLE == 3 && DISP_SEC_CHECK == 4 && DISP_SEC_FD_TO_SEC_HDL == 5);
static_assert(__same_type(typeof(&mtk_disp_secure_cb), void (*)(struct cmdq_cb_data)));
static_assert(__same_type(typeof(disp_sec_cb.cb),
		int (*)(int, struct cmdq_pkt *, resource_size_t, struct dma_buf *)));
static_assert(__same_type(typeof(disp_mtee_cb.cb),
		int (*)(int, int, struct mtk_drm_gem_obj *, struct cmdq_pkt *,
			struct mtk_ddp_comp *, u32, u32, u32, u32, u32)));
#undef STOCK305_DISP_FIELD

#endif
