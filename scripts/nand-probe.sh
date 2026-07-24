#!/usr/bin/env bash
# Load the out-of-tree MTD + Rockchip NFC stack on the running box and enable the NAND
# controller via a runtime DT overlay, to probe the chip WITHOUT a kernel rebuild.
# (Armbian ships CONFIG_MTD=n, so the whole stack is loaded as modules.)
#
#   BOX_HOST=192.168.1.215 ECC=16 ./scripts/nand-probe.sh
#
# Needs SSH key auth and modules built:
#   docker run --rm -v $PWD:/work -w /work rk322x-xbuild env/build-mtd-modules.sh
#   (plus bch.ko — see drivers/nand-rk322x/FINDINGS.md)
set -euo pipefail
HOST=${BOX_HOST:?set BOX_HOST=<ip>}
USER=${BOX_USER:-root}
ECC=${ECC:-16}
STEP=${STEP:-1024}
REPO=$(cd "$(dirname "$0")/.." && pwd)
S=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15)
H="$USER@$HOST"

REL=$(ssh "${S[@]}" "$H" 'uname -r')
ssh "${S[@]}" "$H" "mkdir -p /lib/modules/$REL/extra/nand"
scp -O "${S[@]}" "$REPO"/drivers/nand-rk322x/modules/*.ko "$H:/lib/modules/$REL/extra/nand/"

ssh "${S[@]}" "$H" ECC="$ECC" STEP="$STEP" 'bash -s' <<'EOF'
cd /lib/modules/$(uname -r)/extra/nand
rmmod rockchip_nand_controller nand nandcore mtd bch 2>/dev/null || true
[ -d /sys/kernel/config/device-tree/overlays/nand ] && rmdir /sys/kernel/config/device-tree/overlays/nand || true
for m in bch mtd nandcore nand; do insmod ./$m.ko 2>/dev/null || echo "insmod $m failed"; done
cat > /tmp/nand.dts <<DTS
/dts-v1/;
/plugin/;
/ { fragment@0 { target-path = "/nand-controller@30030000"; __overlay__ {
  status = "okay"; #address-cells = <1>; #size-cells = <0>;
  nand@0 { reg = <0>; nand-ecc-mode = "hw"; nand-ecc-strength = <$ECC>; nand-ecc-step-size = <$STEP>; };
}; }; };
DTS
dtc -@ -I dts -O dtb -o /tmp/nand.dtbo /tmp/nand.dts 2>/dev/null
mkdir -p /sys/kernel/config/device-tree/overlays/nand
cat /tmp/nand.dtbo > /sys/kernel/config/device-tree/overlays/nand/dtbo
dmesg -C >/dev/null 2>&1
insmod ./rockchip-nand-controller.ko 2>/dev/null || true
sleep 2
echo "=== dmesg ==="; dmesg | grep -iE "nand|nfc|mtd|ecc|30030000" | tail -20
echo "=== /proc/mtd ==="; cat /proc/mtd 2>/dev/null || echo none
EOF
