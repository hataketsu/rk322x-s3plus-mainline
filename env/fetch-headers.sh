#!/usr/bin/env bash
# Fetch Armbian linux-headers matching a target kernel and unpack as a build-ready KDIR.
#
# The out-of-tree modules must build against headers whose vermagic matches the kernel
# that will *run* them. Two supported sources:
#
#   1) APT pool on apt.armbian.com   (current Armbian version, e.g. 26.5.x)
#   2) A .deb you place in env/       (for pinning an older version, e.g. 24.2.5 / 6.6.22)
#
# Usage:
#   fetch-headers                      # auto: newest linux-headers-current-rockchip from apt
#   fetch-headers /work/env/linux-headers-current-rockchip_24.2.5_armhf.deb   # pin a local deb
#
set -euo pipefail
KDIR=${KDIR:-/kdir}
DEB=${1:-}

mkdir -p "$KDIR"

if [[ -z "$DEB" ]]; then
    echo ">> fetching newest linux-headers-current-rockchip (armhf) from apt.armbian.com"
    apt-get update -o Acquire::Check-Valid-Until=false \
        -o Dir::Etc::sourcelist=<(echo "deb [trusted=yes] http://apt.armbian.com bookworm main") \
        -o Dir::Etc::sourceparts=- >/dev/null 2>&1 || true
    cd /tmp
    apt-get download linux-headers-current-rockchip:armhf \
        -o Dir::Etc::sourcelist=<(echo "deb [trusted=yes] http://apt.armbian.com bookworm main") \
        -o Dir::Etc::sourceparts=-
    DEB=$(ls -1 /tmp/linux-headers-*.deb | head -1)
fi

echo ">> unpacking $DEB"
dpkg-deb -x "$DEB" /tmp/hdr
SRC=$(ls -d /tmp/hdr/usr/src/linux-headers-* | head -1)
rm -rf "$KDIR"; cp -a "$SRC" "$KDIR"

# prepare for out-of-tree module builds (cross)
make -C "$KDIR" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules_prepare || true
echo ">> KDIR ready: $KDIR  (kernel $(cat "$KDIR/include/config/kernel.release" 2>/dev/null || echo '?'))"
