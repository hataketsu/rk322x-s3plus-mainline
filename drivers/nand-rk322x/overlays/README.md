# RK322x NAND device-tree overlays

Two runtime overlays for the Kiwibox S3 Plus. Exactly one of them may be active
at a time — they claim the same controller node for different drivers. Select it
with `user_overlays=` in `/boot/armbianEnv.txt` and reboot.

| overlay | driver it enables | gives you |
|---|---|---|
| `nand-en` | mainline `rockchip-nand-controller` | `/dev/mtd0`, raw MTD access |
| `nand-vendor` | ported vendor `rknand.ko` | `/dev/rknand0`, the FTL block device |

Build and install:

```sh
dtc -@ -I dts -O dtb -o nand-en.dtbo nand-en.dts
cp nand-en.dtbo /boot/overlay-user/
sed -i 's/^user_overlays=.*/user_overlays=nand-en/' /boot/armbianEnv.txt
```

## The pinctrl list is load-bearing

`pinctrl-0` names the flash pin groups by phandle, read from the running kernel:

```sh
for g in /proc/device-tree/pinctrl/flash/flash-*; do
  printf '%-12s 0x%s\n' "$(basename $g)" \
    "$(python3 -c "import struct;print('%x'%struct.unpack('>I',open('$g/phandle','rb').read())[0])")"
done
```

On this box: ale `0xa7`, bus8 `0xab`, cle `0xa8`, cs0 `0xa2`, dqs `0xac`,
rdn `0xaa`, rdy `0xa6`, wrn `0xa9`, **wp `0xad`**. Phandles are assigned when the
DT is built, so re-read them after any kernel/DTB update instead of copying the
numbers above.

Two pins were each missing at some point, with very different symptoms:

- **No pinctrl at all** — the data/control pins stay unmuxed, READID reads
  floating 0xFF, and the chip is never detected (`nand: No NAND device found`).
- **`flash-wp` missing** — everything *reads* correctly, so the setup looks
  healthy, but WP# is never driven and the chip silently refuses every PROGRAM
  and ERASE. The mainline driver reports `-EIO` on erase/write; the vendor FTL
  is worse, because its programs return status PASS while leaving the cells
  erased, so `FlashMakeFactorBbt` reads its own test pattern back as `0xffffffff`
  and marks all 4096 blocks bad. Keep `0xad` in the list.
