# Track A — RTL8189FS (SDIO `024C:8179`) on kernel 6.6

The in-tree `rtl8xxxu` is USB-only and does not bind this SDIO part. We build the
out-of-tree SDIO driver against the pinned KDIR.

## Source

Vendored as a git submodule (recommended tree, actively patched for 6.x):

```bash
git submodule add https://github.com/jethome-ru/rtl8189FS src
# alternatives: lwfinger/rtl8189es (rtl8189ES), morrownr/rtl8189es (rtl8189ES)
```

A **reference build + firmware** also exists in the vendor 4.4 rootfs — carve it for
comparison:

```bash
./scripts/extract-rom.sh images/Armbian_21.05.1_*_legacy_4.4.194.img.xz
ls extracted/*/lib/modules/4.4.194-rk322x/ | grep 8189
ls extracted/*/lib/firmware/rtlwifi/
```

## Build (cross, via env container)

```bash
make -C ../../env toolchain          # once
make -C ../../env headers            # populates env/kdir
make                                 # builds 8189es (rtl8189ES).ko for env/kdir
```

## Install on box

```bash
BOX_HOST=192.168.1.94 ../../scripts/rsh.sh 'mkdir -p /lib/modules/$(uname -r)/extra'
scp 8189es (rtl8189ES).ko root@192.168.1.94:/lib/modules/$(uname -r)/extra/
BOX_HOST=192.168.1.94 ../../scripts/rsh.sh 'depmod -a && modprobe 8189es (rtl8189ES) && ip -br link'
```

Then connect:

```bash
nmcli dev wifi connect "<SSID>" password "<PASS>"
```

## Notes

- Module **vermagic must match the running kernel** — build against the exact KDIR for
  the kernel the box runs (see `docs/hardware.md` version caveat).
- If `depmod`/`modprobe` reports vermagic mismatch, the KDIR version is wrong.
