# SPDX-License-Identifier: GPL-2.0
include $(KERNEL_SRC)/$(DEVICE_MODULES_REL_DIR)/Makefile.include

stock305_rules := $(KERNEL_SRC)/$(DEVICE_MODULES_REL_DIR)/drivers/dma-buf/heaps/stock-platform-abi
stock305_symvers := $(abspath $(OUT_DIR)/$(M)/stock-platform.symvers)

.PHONY: modules modules_install compile_commands.json
modules:
	mkdir -p $(dir $(stock305_symvers))
	awk -f $(stock305_rules)/merge-symvers.awk stock-platform.symvers $(EXTRA_SYMBOLS) > $(stock305_symvers)
	$(MAKE) -C $(KERNEL_SRC) M=$(M) KBUILD_EXTRA_SYMBOLS="$(stock305_symvers)" $(@)

modules_install compile_commands.json:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) KBUILD_EXTRA_SYMBOLS="$(stock305_symvers)" $(@)
