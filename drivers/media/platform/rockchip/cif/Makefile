# SPDX-License-Identifier: GPL-2.0
ccflags-y += -include linux/rk-v4l2-compat.h
obj-$(CONFIG_VIDEO_ROCKCHIP_CIF) += video_rkcif.o
video_rkcif-objs += dev.o \
			capture.o \
			mipi-csi2.o \
			cif-luma.o \
			hw.o \
			subdev-itf.o \
			procfs.o \
			cif-scale.o \
			common.o \
			cif-tools.o \
			cif-rockit.o
