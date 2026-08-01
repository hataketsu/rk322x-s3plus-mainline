# Track A — RTL8189ES (SDIO `024C:8179`) on kernel 6.6

Chip confirmed as **Realtek RTL8189ES** (vendor 4.4 module `8189es`, dir
`rockchip_wlan/rtl8189es`, firmware `rkwifi/wifi_efuse_8189e.map`). The in-tree
`rtl8xxxu` is USB-only and does not bind this SDIO part, so we build the out-of-tree
`8189es` module. The box already enumerates the SDIO card under 6.6 (dwmmc + vendor DT),
so **only the driver module is missing** — no controller/pinmux/pwrseq work needed
(unlike the S805 bring-up, `docs/`).

## Source

`src/` is a **git submodule**, not vendored — the tree is ~33 MB and upstream is
maintained, so the repo pins a commit instead of copying it.

| | |
|---|---|
| Upstream | <https://github.com/jwrdegoede/rtl8189ES_linux> |
| Pinned commit | `2d9a8afb5d12de1cfc4ab5ad3d1a61e1937629bd` |
| Driver version | `v5.8.9_35085.20190919` (`include/rtw_version.h`) |

Hans de Goede's fork is the one that tracks current kernels; the Realtek original
does not build on 6.x. It is also the same tree used for the author's mainline
S805 8189ES bring-up, so the build recipe carried over.

```bash
git clone --recursive https://github.com/hataketsu/rk322x-s3plus-mainline
# or, in an existing clone:
git submodule update --init drivers/wifi-rtl8189es/src
```

To move to a different upstream revision, check it out inside `src/` and commit
the resulting gitlink — that is the only thing this repo stores for it.

Vendor reference build + firmware (carved from the 4.4 ROM, for comparison only):

```
extracted/*/lib/modules/4.4.194-rk322x/kernel/drivers/net/wireless/rockchip_wlan/rtl8189es/8189es.ko
extracted/*/lib/firmware/rkwifi/wifi_efuse_8189e.map
```

## Build (cross, via env container)

No matching `linux-headers` .deb exists for Armbian 24.2.5 / 6.6.22, so the KDIR is
built from mainline source + the box's own `.config`. `CONFIG_MODVERSIONS=n` on the box,
so only vermagic must match — no `Module.symvers` needed.

```bash
make -C ../../env toolchain      # once — armhf cross image
make -C ../../env kdir-src       # KDIR from source, pinned to 6.6.22 vermagic
make -C ../../env shell          # inside container:
  make -C drivers/wifi-rtl8189es KDIR=/work/env/kdir
```

Built `8189es.ko` must report the box's exact vermagic:

```
6.6.22-current-rockchip SMP mod_unload ARMv7 p2v8
```

## Install on box

```bash
scp 8189es.ko root@192.168.1.94:/lib/modules/6.6.22-current-rockchip/extra/
BOX_HOST=192.168.1.94 ../../scripts/rsh.sh 'depmod -a'
# load with power-save disabled (reused from the S805 8189ES work)
BOX_HOST=192.168.1.94 ../../scripts/rsh.sh 'modprobe cfg80211; insmod /lib/modules/$(uname -r)/extra/8189es.ko rtw_power_mgnt=0 rtw_ips_mode=0; ip -br link'
```

Also copy the firmware if the driver needs it:
`extracted/*/lib/firmware/rkwifi/` → box `/lib/firmware/rkwifi/`.

Then connect:

```bash
nmcli dev wifi connect "<SSID>" password "<PASS>"
```

## Notes

- vermagic mismatch on `insmod` ⇒ KDIR kernelrelease is wrong (check the `localversion`
  file used by `env/build-kdir.sh` = `-current-rockchip`).
- Load params `rtw_power_mgnt=0 rtw_ips_mode=0` disable IPS power-save (stability), per
  the S805 8189ES bring-up. Needs `cfg80211` + `rfkill` loaded.
