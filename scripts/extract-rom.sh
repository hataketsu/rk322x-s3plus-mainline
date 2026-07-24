#!/usr/bin/env bash
# Carve reference blobs out of a vendor Armbian .img.xz (rootfs is ext4, unmountable on
# macOS). Runs debugfs inside the rk322x-xbuild container, so the host needs no ext4 tools.
#
#   ./scripts/extract-rom.sh images/Armbian_21.05.1_..._legacy_4.4.194.img.xz
#
# Output → extracted/<image-stem>/  (gitignored). Pulls kernel modules, WiFi firmware,
# device tree and boot bits used by Track A (wifi) and Track B (nand) RE.
set -euo pipefail

IMG=${1:?usage: extract-rom.sh <image.img.xz>}
IMAGE=${XBUILD_IMAGE:-rk322x-xbuild}
REPO=$(cd "$(dirname "$0")/.." && pwd)
STEM=$(basename "$IMG" | sed 's/\.img\.xz$//; s/\.img$//')
OUT="extracted/$STEM"

command -v docker >/dev/null || { echo "docker required (colima/docker)"; exit 1; }

docker run --rm -v "$REPO":/work -w /work "$IMAGE" bash -euo pipefail -c '
IMG="'"$IMG"'"; OUT="'"$OUT"'"
mkdir -p "$OUT"
echo ">> decompressing $IMG"
case "$IMG" in
  *.xz) xz -dc "$IMG" > /tmp/rom.raw ;;
  *)    cp "$IMG" /tmp/rom.raw ;;
esac

# first MBR partition start LBA (LE u32 @ 0x1C6) -> byte offset
START=$(od -An -tu4 -j 454 -N4 /tmp/rom.raw | tr -d " ")
OFF=$((START * 512))
echo ">> rootfs partition starts at sector $START ($OFF bytes)"
dd if=/tmp/rom.raw of=/tmp/rootfs.img bs=1M skip=$((OFF/1048576)) status=none

echo ">> rootfs:"; dumpe2fs -h /tmp/rootfs.img 2>/dev/null | grep -E "Filesystem volume|Block count" || true

# rdump target paths (ignore missing)
rdump() { debugfs -R "rdump \"$1\" \"$OUT$2\"" /tmp/rootfs.img 2>/dev/null || true; }
mkdir -p "$OUT/lib" "$OUT/boot"
for KMOD in $(debugfs -R "ls -l /lib/modules" /tmp/rootfs.img 2>/dev/null | awk "{print \$NF}" | grep -E "^[0-9]"); do
  echo ">> modules dir: $KMOD"
done
# whole trees we care about
debugfs -R "rdump /lib/modules \"$OUT/lib/\""  /tmp/rootfs.img 2>/dev/null || true
debugfs -R "rdump /lib/firmware \"$OUT/lib/\"" /tmp/rootfs.img 2>/dev/null || true
debugfs -R "rdump /boot \"$OUT/\""             /tmp/rootfs.img 2>/dev/null || true
debugfs -R "rdump /usr/sbin \"$OUT/\""         /tmp/rootfs.img 2>/dev/null || true

echo ">> key blobs found:"
find "$OUT" -iname "rknand*.ko" -o -iname "*8189*" -o -iname "*.dtb" 2>/dev/null | sed "s|^|   |"
'
echo ">> done. Extracted under $REPO/$OUT"
