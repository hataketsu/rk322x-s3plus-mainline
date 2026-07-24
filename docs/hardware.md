# Hardware & probe findings — Kiwibox S3 Plus (RK322x)

All data below is measured from **this unit**, not datasheets.

## SoC / board

```
Machine model : Generic RK322x TV Box board  (rk322x-box)
SoC           : Rockchip RK3229, Cortex-A7 quad, armv7l
RAM           : 2 GB DDR3  (ddrbin V1.11 20200910)
Armbian       : BOARD=rk322x-box LINUXFAMILY=rockchip BRANCH=current VERSION=24.2.5
Running kernel: 6.6.22-current-rockchip  (booted from SD /dev/mmcblk0p1)
```

## Serial console

- UART2 → Linux `ttyS2`, **115200 8N1**, `console=ttyS2,115200`.
- macOS: adapter enumerated as `/dev/cu.usbserial-*`; `screen /dev/cu.usbserial-XXXX 115200`.

## WiFi — RTL8189FS (Track A)

```
SDIO bus   : mmc1 (non-removable), "new high speed SDIO card at address 0001"
SDIO id    : VendorID 0x024C (Realtek)  DeviceID 0x8179
MODALIAS   : sdio:c07v024Cd8179
```

- No in-tree match: kernel 6.6 `modprobe -c` has `024C` aliases only for
  8723cs / r8723bs / rtw88_8822bs — **nothing for `d8179`**.
- In-tree `rtl8xxxu` is **USB-only**, does not bind SDIO 8189FS.
- Driver needed: out-of-tree `rtl8189fs` (SDIO), e.g. the `lwfinger`/`jethome` trees.
- Firmware + a reference build of the module also live in the vendor 4.4 rootfs
  (`/lib/modules/4.4.194-rk322x/…/8189fs.ko`, `/lib/firmware/rtlwifi/…`).

## NAND (Track B)

```
Device-tree node : nand-controller@30030000   (present under 6.6, no driver bound)
Under 4.4        : rknandbase v1.2 2018-05-08
                   rknand 30030000.nandc: rknand_probe clk rate = 150000000
                   rknand0: p1
                   rknand vendor storage init ok !
Under 6.6        : /proc/mtd empty, no /dev/mtd*, no /dev/rknand* — unusable
On-chip size     : ~8 GB raw NAND (vendor "SU08G"-class); usable after vendor blocks
```

- The 4.4 driver is the **closed `rknand` blob** (`rknand.ko` / `rk_nand`), precompiled
  for the 4.4 ABI. Not recompilable for 6.6.
- Mainline has an NFC driver `rockchip-nand-controller` (`drivers/mtd/nand/raw/`), but it
  targets Rockchip's *open* NFC, not necessarily this box's controller variant — this is
  the core RE question for Track B (see `nand-boot.md`).

## SD image reference

| Image | SHA-256 | Kernel |
|---|---|---|
| `Armbian_21.05.1_Rk322x-box_buster_legacy_4.4.194.img.xz` | `656270536d1f11550eacd535d94fdc09ed7333192a1907f4d3ad8e29b19566d6` | 4.4.194 (has rknand + 8189fs) |
| `Armbian_24.2.5_Rk322x-box_bookworm_current_6.6.22_xfce_desktop.img.xz` | (build byte-identical to Armbian 24.2.5 official) | 6.6.22 (currently running) |

Armbian SD image partition layout (both): MBR, single ext4 `armbi_root` at **LBA 0x2000
(4 MiB offset)**; idbloader at sector 64, `u-boot.itb` after. Bootloader strings in the
current image: `U-Boot SPL 2022.04-armbian`, `DDR Version V1.11`.

Multitool image: `nand-controller` init + steP-nand scripts; download
`https://apt.undo.it:7243/multitool-rk322x.xz` (79 MB).

## Toolchain / kernel-version caveat

Box apt now only offers `linux-headers-current-rockchip` **26.5.1** (a newer Armbian),
while the running kernel is **6.6.22** (24.2.5). Installing that header package natively
would mismatch the running kernel (vermagic) and produced modules would not load.
→ The cross-build env **pins headers to 6.6.22**; see `env/`.
