# SPDX-License-Identifier: GPL-2.0
# Parent/config/package activation is deferred; all native build checks skipped.
ccflags-y += -I$(srctree) -I$(srctree)/kernel
obj-$(CONFIG_XIAOMI_MIGT) += migt.o
migt-y := module.o owners.o task_policy.o runtime.o frequency.o history.o \
	frame.o pipeline.o render.o controls.o boost.o user-api.o worker.o \
	subsystem-lifecycle.o module-lifecycle.o empty-controls.o glk-update-util.o
