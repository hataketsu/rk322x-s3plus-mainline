# Cross-build environment control.
#
#   make toolchain     build the cross-build docker image
#   make headers       fetch + unpack matching kernel headers into ./kdir
#   make shell         drop into the toolchain container (repo mounted at /work)
#
# Override the target kernel:  make toolchain KVER=6.6.22-current-rockchip

IMAGE ?= rk322x-xbuild
KVER  ?= 6.6.22-current-rockchip
REPO  := $(abspath ..)

.PHONY: toolchain headers shell clean

toolchain:
	docker build -t $(IMAGE) --build-arg KVER=$(KVER) .

# Fetch headers into ./kdir (persisted on host, gitignored). Pin an older version by
# dropping its .deb into this dir and passing DEB=env/<file>.deb
headers:
	docker run --rm -v $(REPO):/work -w /work $(IMAGE) \
		bash -c 'KDIR=/work/env/kdir fetch-headers $(if $(DEB),/work/$(DEB),)'

# Build a module-ready KDIR from mainline source + the box's .config, pinned to the
# running kernel's vermagic. Use this when no matching headers .deb exists (our case:
# Armbian 24.2.5 / 6.6.22). CONFIG_MODVERSIONS=n on the box, so no symvers needed.
KVER_ONLY ?= 6.6.22
LOCALVER  ?= -current-rockchip
CONFIG    ?= env/config/config-$(KVER_ONLY)$(LOCALVER)
kdir-src:
	docker run --rm -v $(REPO):/work -w /work $(IMAGE) \
		bash -c 'KDIR=/work/env/kdir env/build-kdir.sh $(KVER_ONLY) $(LOCALVER) /work/$(CONFIG)'

shell:
	docker run --rm -it -v $(REPO):/work -w /work \
		-e KDIR=/work/env/kdir $(IMAGE) bash

clean:
	rm -rf kdir *.deb
