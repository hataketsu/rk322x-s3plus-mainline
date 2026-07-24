#!/usr/bin/env bash
# Build a module-ready KDIR from mainline kernel source + the box's own .config,
# pinned to the running kernel's vermagic. Used when a matching linux-headers .deb
# is unavailable (the case for Armbian 24.2.5 / 6.6.22 — apt only offers a newer ver).
#
# Runs inside the rk322x-xbuild container (has the armhf cross-toolchain).
# The box has CONFIG_MODVERSIONS=n, so no Module.symvers/CRC matching is needed —
# only vermagic (kernelrelease + config flags) must match. We force the kernelrelease
# via a `localversion` file and reuse the box's exact .config.
#
#   Inside container (make -C env shell):
#     KDIR=/work/env/kdir env/build-kdir.sh 6.6.22 -current-rockchip \
#          /work/env/config/config-6.6.22-current-rockchip
set -euo pipefail

KVER=${1:?kernel version e.g. 6.6.22}
LOCALVER=${2:?localversion e.g. -current-rockchip}
CONFIG=${3:?path to .config}
KDIR=${KDIR:-/work/env/kdir}
ARCH=arm
CROSS=arm-linux-gnueabihf-
MAJ=${KVER%%.*}

echo ">> fetching linux-$KVER source"
mkdir -p /tmp/ksrc && cd /tmp/ksrc
if [[ ! -f linux-$KVER.tar.xz ]]; then
    curl -fSL -o linux-$KVER.tar.xz \
        "https://cdn.kernel.org/pub/linux/kernel/v${MAJ}.x/linux-$KVER.tar.xz"
fi
rm -rf linux-$KVER && tar xf linux-$KVER.tar.xz
cd linux-$KVER

echo ">> applying box .config + localversion ($LOCALVER)"
cp "$CONFIG" .config
printf '%s' "$LOCALVER" > localversion

echo ">> configure + modules_prepare (cross $ARCH)"
make ARCH=$ARCH CROSS_COMPILE=$CROSS olddefconfig
make ARCH=$ARCH CROSS_COMPILE=$CROSS modules_prepare -j"$(nproc)"

REL=$(cat include/config/kernel.release)
echo ">> kernelrelease = $REL   (want ${KVER}${LOCALVER})"
[[ "$REL" == "${KVER}${LOCALVER}" ]] || echo "!! WARNING: kernelrelease mismatch — vermagic will not match the box"

echo ">> publishing KDIR -> $KDIR"
rm -rf "$KDIR"; mkdir -p "$(dirname "$KDIR")"
cp -a /tmp/ksrc/linux-$KVER "$KDIR"
echo ">> KDIR ready: $KDIR"
