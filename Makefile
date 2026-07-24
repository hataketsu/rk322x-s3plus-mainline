# Top-level convenience targets. See README.md.

.PHONY: toolchain headers extract wifi shell help

help:
	@echo "targets:"
	@echo "  toolchain   build the cross-build docker image"
	@echo "  headers     fetch + unpack matching kernel headers (env/kdir)"
	@echo "  extract     carve vendor 4.4 ROM (set IMG=images/....img.xz)"
	@echo "  wifi        build the RTL8189FS module"
	@echo "  shell       enter the toolchain container"

toolchain:
	$(MAKE) -C env toolchain

headers:
	$(MAKE) -C env headers $(if $(DEB),DEB=$(DEB),)

IMG ?= $(firstword $(wildcard images/*legacy_4.4*.img.xz))
extract:
	./scripts/extract-rom.sh $(IMG)

wifi:
	$(MAKE) -C drivers/wifi-8189fs

shell:
	$(MAKE) -C env shell
