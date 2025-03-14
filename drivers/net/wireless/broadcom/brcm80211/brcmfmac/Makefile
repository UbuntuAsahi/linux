# SPDX-License-Identifier: ISC
#
# Makefile fragment for Broadcom 802.11 Networking Device Driver
#
# Copyright (c) 2010 Broadcom Corporation
#

ccflags-y += \
	-I $(src) \
	-I $(src)/../include

obj-$(CONFIG_BRCMFMAC) += brcmfmac.o
brcmfmac-objs += \
		cfg80211.o \
		chip.o \
		fwil.o \
		fweh.o \
		p2p.o \
		proto.o \
		common.o \
		core.o \
		firmware.o \
		fwvid.o \
		feature.o \
		btcoex.o \
		vendor.o \
		pno.o \
		join_param.o \
		scan_param.o \
		xtlv.o \
		interface_create.o

brcmfmac-$(CONFIG_BRCMFMAC_PROTO_BCDC) += \
		bcdc.o \
		fwsignal.o
brcmfmac-$(CONFIG_BRCMFMAC_PROTO_MSGBUF) += \
		commonring.o \
		flowring.o \
		msgbuf.o
brcmfmac-$(CONFIG_BRCMFMAC_SDIO) += \
		sdio.o \
		bcmsdh.o
brcmfmac-$(CONFIG_BRCMFMAC_USB) += \
		usb.o
brcmfmac-$(CONFIG_BRCMFMAC_PCIE) += \
		pcie.o
brcmfmac-$(CONFIG_BRCMDBG) += \
		debug.o
brcmfmac-$(CONFIG_BRCM_TRACING) += \
		tracepoint.o
brcmfmac-$(CONFIG_OF) += \
		of.o
brcmfmac-$(CONFIG_DMI) += \
		dmi.o
brcmfmac-$(CONFIG_ACPI) += \
		acpi.o

ifeq ($(CONFIG_BRCMFMAC),m)
obj-m += wcc/
obj-m += cyw/
obj-m += bca/
else
brcmfmac-$(CONFIG_BRCMFMAC) += wcc/core.o
brcmfmac-$(CONFIG_BRCMFMAC) += cyw/core.o
brcmfmac-$(CONFIG_BRCMFMAC) += bca/core.o
endif
