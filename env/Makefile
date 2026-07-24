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

shell:
	docker run --rm -it -v $(REPO):/work -w /work \
		-e KDIR=/work/env/kdir $(IMAGE) bash

clean:
	rm -rf kdir *.deb
