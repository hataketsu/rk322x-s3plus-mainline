# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build of the Rockchip vendor rk_nand MLC-NAND FTL block driver,
# forward-ported from the RK 5.10 BSP to mainline/Armbian 6.6 (armv7 / rk322x).
# Produces rknand.ko, exposing the 8GB MLC NAND as /dev/rknand0 via the closed
# FTL blob (FtlRead/FtlWrite/rk_ftl_init).
#
# Build (inside the rk322x-xbuild container, repo mounted at /work):
#   make -C /work/env/kdir \
#        M=/work/drivers/nand-rk322x/rknand-port \
#        ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
#
# or simply, with KDIR pointed at the pinned 6.6 tree:
#   make KDIR=/work/env/kdir

obj-m := rknand.o
rknand-y := rk_nand_base.o rk_nand_blk.o rknand_shims.o

# ---- FTL blob selection (replicates the vendor Makefile) --------------------
# The box is armv7 (CONFIG_ARM=y), non-Thumb kernel, so link the 32-bit ARM
# blobs: rk_zftl_arm32.o exports the Ftl* API and dispatches into the FTL-v5
# core in rk_ftlv5_arm32.o (matches the box's FTL 5.0.5x). Do NOT edit the .S.
# NB: the vendor tree nested these under an rk_ftl.o composite; kbuild does not
# support composite-in-composite, so link the blob objects into rknand directly.
ifdef CONFIG_THUMB2_KERNEL
rknand-y += rk_ftl_arm_v7_thumb.o
else
rknand-$(CONFIG_ARM)   += rk_zftl_arm32.o rk_ftlv5_arm32.o
rknand-$(CONFIG_ARM64) += rk_zftl_arm64.o rk_ftlv5_arm64.o
endif

# ---- convenience wrapper for a direct `make` (ignored by kbuild) ------------
KDIR ?= /home/hataketsu/Projects/s3plus/env/kdir
ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
