# SPDX-License-Identifier: GPL-2.0-only
# Leaf metadata only. Parent/config/package activation is still deferred.
ccflags-y += -I$(srctree) -I$(srctree)/kernel -I$(src)
obj-$(CONFIG_XIAOMI_METIS) += metis.o
metis-y := metis-state.o metis-classify.o metis-callbacks.o metis-lowlt.o \
	metis-blocked.o metis-binder-request.o metis-link.o metis-common.o \
	metis-coldstart.o metis-vip-queue.o metis-extra-controls.o \
	metis-registry.o metis-walt-policy.o metis-wakeup.o \
	metis-coldstart-policy.o metis-deadline.o metis-schlat.o metis-io.o \
	metis-monitor.o metis-cluster-boost.o metis-lock-wait.o metis-controls.o \
	metis-final-control-affinity.o metis-final-control-persist.o \
	metis-final-control-api.o metis-final-control-comm.o \
	metis-final-control-history.o metis-final-link.o metis-final-link-spin.o \
	metis-final-scheduler-affinity.o metis-final-scheduler-load.o \
	metis-final-scheduler-monitor.o metis-final-scheduler-placement.o \
	metis-final-boost.o metis-final-important.o metis-final-owners.o \
	metis-final-tables.o metis-final-trace.o metis-final-module.o \
	metis-final-exports.o metis-stock-entries.o metis-stock-context.o \
	metis-stock-util.o metis-stock-util-call.o
