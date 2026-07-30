# Cross-build the rknand read-only FTL (Phase B) module against the pinned KDIR.
#
#   make            # build rk-ftl.ko
#   make KDIR=...   # override kernel headers/source dir
#   make clean
#
# Run inside the toolchain container:  make -C ../../../env shell  -> then  make
#
# Same recipe as the wifi/nand out-of-tree modules:
#  - ARCH=arm, CROSS_COMPILE=arm-linux-gnueabihf-
#  - KDIR = mainline 6.6.x + box config, source-built (no full vmlinux)
#  - CONFIG_MODVERSIONS=n on the box.
#  - KBUILD_MODPOST_WARN=1 : the KDIR has no Module.symvers, so modpost cannot
#    verify kernel symbols (register_mtd_blktrans, mtd_read_oob, ...). The box
#    provides them all; downgrade "undefined symbol" to a warning and link.

KDIR          ?= $(abspath ../../../env/kdir)
ARCH          ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-

obj-m += rk-ftl.o

.PHONY: all clean

all:
	@test -f $(KDIR)/Makefile || { echo "no KDIR at $(KDIR) - run: make -C ../../../env kdir-src"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(CURDIR) -j$$(nproc) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		KBUILD_MODPOST_WARN=1 \
		modules
	@echo ">> built: $$(ls -la rk-ftl.ko 2>/dev/null)"
	@echo ">> vermagic: $$(modinfo -F vermagic rk-ftl.ko 2>/dev/null)"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean 2>/dev/null || true
	rm -f *.ko *.o *.mod *.mod.c .*.cmd modules.order Module.symvers
	rm -rf .tmp_versions
