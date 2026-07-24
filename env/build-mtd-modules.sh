#!/usr/bin/env bash
# Build the MTD + Rockchip NFC stack as loadable modules against the pinned KDIR, so the
# NAND can be inspected on the running kernel (which ships CONFIG_MTD=n) without a full
# kernel rebuild. Pair with a runtime DT overlay that enables nand-controller@30030000.
#
# Run inside the rk322x-xbuild container:
#   docker run --rm -v $PWD:/work -w /work rk322x-xbuild env/build-mtd-modules.sh
set -euo pipefail
KDIR=${KDIR:-/work/env/kdir}
ARCH=arm; CROSS=arm-linux-gnueabihf-
OUT=/work/drivers/nand-rk322x/modules
mkdir -p "$OUT"
cd "$KDIR"

echo ">> enabling MTD + Rockchip NFC as modules in KDIR .config"
scripts/config \
  --module MTD \
  --module MTD_BLKDEVS \
  --module MTD_BLOCK \
  --module MTD_NAND_CORE \
  --module MTD_RAW_NAND \
  --module MTD_NAND_ROCKCHIP \
  --enable  MTD_NAND_ECC_SW_HAMMING \
  --enable  MTD_NAND_ECC_SW_BCH
make ARCH=$ARCH CROSS_COMPILE=$CROSS olddefconfig
make ARCH=$ARCH CROSS_COMPILE=$CROSS modules_prepare

echo ">> building module objects"
KO="drivers/mtd/mtd.ko \
    drivers/mtd/mtdblock.ko \
    drivers/mtd/nand/nandcore.ko \
    drivers/mtd/nand/raw/nand.ko \
    drivers/mtd/nand/raw/rockchip-nand-controller.ko"
make ARCH=$ARCH CROSS_COMPILE=$CROSS -j"$(nproc)" KBUILD_MODPOST_WARN=1 $KO

echo ">> collecting .ko"
for f in $KO; do [ -f "$f" ] && cp -v "$f" "$OUT/"; done
ls -la "$OUT"
