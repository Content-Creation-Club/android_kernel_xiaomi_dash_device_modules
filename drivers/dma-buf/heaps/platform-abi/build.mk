# SPDX-License-Identifier: GPL-2.0
include $(KERNEL_SRC)/$(DEVICE_MODULES_REL_DIR)/Makefile.include

provider_rules := $(KERNEL_SRC)/$(DEVICE_MODULES_REL_DIR)/drivers/dma-buf/heaps/platform-abi
provider_symvers := $(abspath $(OUT_DIR)/$(M)/platform.symvers)

.PHONY: modules modules_install compile_commands.json
modules:
	mkdir -p $(dir $(provider_symvers))
	awk -f $(provider_rules)/merge-symvers.awk platform.symvers $(EXTRA_SYMBOLS) > $(provider_symvers)
	$(MAKE) -C $(KERNEL_SRC) M=$(M) KBUILD_EXTRA_SYMBOLS="$(provider_symvers)" $(@)

modules_install compile_commands.json:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) KBUILD_EXTRA_SYMBOLS="$(provider_symvers)" $(@)
