# SPDX-License-Identifier: GPL-2.0-only

ccflags-y += -I$(srctree)/techpack/camera-xiaomi-lmi/include/uapi
ccflags-y += -I$(srctree)/techpack/camera-xiaomi-lmi/drivers/cam_req_mgr
ccflags-y += -I$(srctree)/techpack/camera-xiaomi-lmi/drivers/cam_smmu/
ccflags-y += -I$(srctree)/techpack/camera-xiaomi-lmi/drivers/cam_sync
ccflags-y += -I$(srctree)/techpack/camera-xiaomi-lmi/drivers/cam_utils
ccflags-y += -I$(src)

obj-$(CONFIG_SPECTRA_CAMERA) += cam_context.o cam_context_utils.o cam_node.o cam_subdev.o
