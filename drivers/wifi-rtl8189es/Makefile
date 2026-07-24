# Cross-build the RTL8189FS SDIO module against the pinned KDIR.
#
#   make            # build (expects env/kdir populated: make -C ../../env headers)
#   make KDIR=...   # override kernel headers dir
#   make clean
#
# Run this inside the toolchain container (make -C ../../env shell) or with a host
# cross-toolchain. ARCH/CROSS_COMPILE default to the rk322x armhf target.

KDIR          ?= $(abspath ../../env/kdir)
ARCH          ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
SRC           ?= src

export ARCH CROSS_COMPILE

.PHONY: all clean

all:
	@test -d $(SRC) || { echo "add driver source first: git submodule add <8189fs repo> $(SRC)"; exit 1; }
	@test -f $(KDIR)/Makefile || { echo "no KDIR at $(KDIR) — run: make -C ../../env headers"; exit 1; }
	$(MAKE) -C $(SRC) KSRC=$(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	@find $(SRC) -name '*.ko' -exec cp {} . \;
	@echo ">> built: $$(ls *.ko 2>/dev/null)"

clean:
	-$(MAKE) -C $(SRC) clean 2>/dev/null
	rm -f *.ko
