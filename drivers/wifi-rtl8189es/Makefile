# Cross-build the RTL8189ES SDIO module against the pinned KDIR.  WORKING recipe.
#
#   make            # build 8189es.ko  (expects env/kdir: make -C ../../env kdir-src)
#   make KDIR=...   # override kernel headers/source dir
#   make clean
#
# Run inside the toolchain container:  make -C ../../env shell  → then  make
#
# Key flags (see README):
#  - CONFIG_PLATFORM_I386_PC=y  : generic Linux profile — enables cfg80211, avoids the
#    Android platform blocks that pull <linux/wlan_plat.h> (RTW_ENABLE_WIFI_CONTROL_FUNC).
#  - KBUILD_MODPOST_WARN=1       : the KDIR is source-built without a full vmlinux, so it
#    has no Module.symvers; modpost can't verify kernel symbols. The box has them all and
#    CONFIG_MODVERSIONS=n, so downgrade "undefined symbol" to a warning and link anyway.
#  - MODULE_NAME=8189es          : output 8189es.ko (else the RK block names it wlan.ko).

KDIR          ?= $(abspath ../../env/kdir)
ARCH          ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
SRC           ?= src

.PHONY: all clean

all:
	@test -f $(SRC)/Makefile || { echo "driver source missing — git submodule update --init $(SRC)"; exit 1; }
	@test -f $(KDIR)/Makefile || { echo "no KDIR at $(KDIR) — run: make -C ../../env kdir-src"; exit 1; }
	$(MAKE) -C $(SRC) -j$$(nproc) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) KSRC=$(KDIR) \
		CONFIG_PLATFORM_I386_PC=y MODULE_NAME=8189es KBUILD_MODPOST_WARN=1 \
		modules
	@cp $(SRC)/8189es.ko . && echo ">> built: $$(ls -la 8189es.ko)"
	@echo ">> vermagic: $$(modinfo -F vermagic 8189es.ko 2>/dev/null)"

clean:
	-$(MAKE) -C $(SRC) clean 2>/dev/null
	rm -f *.ko
