#!/usr/bin/env bash
# Deploy the built 8189es.ko to the box, load it, and make it persist across reboots.
#
#   BOX_HOST=192.168.1.94 ./scripts/deploy-wifi.sh
#
# Requires SSH key auth to the box (see scripts/setup-box-key.sh) and a built module at
# drivers/wifi-rtl8189es/8189es.ko (make -C drivers/wifi-rtl8189es).
set -euo pipefail
HOST=${BOX_HOST:?set BOX_HOST=<ip>}
USER=${BOX_USER:-root}
REPO=$(cd "$(dirname "$0")/.." && pwd)
KO="$REPO/drivers/wifi-rtl8189es/8189es.ko"
FW=$(find "$REPO/extracted" -type d -name rkwifi 2>/dev/null | head -1)
S=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10)
H="$USER@$HOST"

[[ -f "$KO" ]] || { echo "build first: make -C drivers/wifi-rtl8189es"; exit 1; }

REL=$(ssh "${S[@]}" "$H" 'uname -r')
echo ">> box kernel: $REL"
ssh "${S[@]}" "$H" "mkdir -p /lib/modules/$REL/extra /lib/firmware/rkwifi"
scp -O "${S[@]}" "$KO" "$H:/lib/modules/$REL/extra/"
[[ -n "$FW" ]] && scp -O "${S[@]}" "$FW"/*.map "$H:/lib/firmware/rkwifi/" 2>/dev/null || true

ssh "${S[@]}" "$H" 'bash -s' <<'EOF'
set -e
echo "8189es" > /etc/modules-load.d/8189es.conf
printf 'options 8189es rtw_power_mgnt=0 rtw_ips_mode=0\n' > /etc/modprobe.d/8189es.conf
depmod -a
modprobe cfg80211 || true
modprobe 8189es || insmod /lib/modules/$(uname -r)/extra/8189es.ko rtw_power_mgnt=0 rtw_ips_mode=0
echo "=== loaded ==="
lsmod | grep 8189es
ip -br link | grep -i wlan || echo "no wlan iface yet"
EOF
echo ">> done. Connect with:  nmcli dev wifi connect \"<SSID>\" password \"<PASS>\""
