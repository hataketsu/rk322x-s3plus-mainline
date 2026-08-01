# Kiwibox S3 Plus (RK322x) — mainline Armbian from onboard NAND

A Rockchip RK3229 TV box whose NAND and WiFi only ever worked on the vendor's
closed 4.4 kernel now runs **Armbian with kernel 6.6 from its own NAND, with no
SD card**, WiFi included.

```
findmnt /                 /dev/rknand0p4
lsblk                     rknand0 -> p1 p2 p3 p4        (no mmcblk0)
uname -r                  6.6.22-current-rockchip
systemctl is-system-running   running
end0 / wlan0              both up
```

Both blockers are closed:

| Track | Goal | State |
|---|---|---|
| **A — WiFi** | RTL8189ES (SDIO `024C:8179`) on 6.6 | **working** — `wlan0`, WPA2, DHCP, auto-loads |
| **B — NAND** | boot and run 6.6 from onboard NAND | **working** — full chain, rootfs on NAND |

## How it boots

Mainline U-Boot cannot start this board from NAND: its TPL/SPL has no rk322x NAND
support, and the flash is managed by Rockchip's closed FTL rather than raw
MTD/UBI. So the first two stages stay vendor, and everything above them is ours:

```
BootROM
  → idblock            vendor DDR init + miniloader, in reserved physical blocks
  → miniloader         reads the FTL, finds partitions via `parameter`
  → trust.img + uboot.img
  → vendor U-Boot 2017.09
  → extlinux           /extlinux/extlinux.conf on an ext4 partition of the FTL
  → mainline 6.6 kernel + rk322x-box DTB + initrd carrying rknand.ko
  → rootfs on NAND
```

The same closed FTL blob is linked into both the U-Boot stage and our kernel
module, which is what lets U-Boot and Linux agree on the layout.

## Start here

- **[docs/journey/](docs/journey/)** — the write-up series: how the port was done,
  what went wrong, and the lessons. Read this to reproduce or to port to another
  RK322x box.
- **[docs/nand-boot.md](docs/nand-boot.md)** — the boot chain as a procedure:
  layout, how to build each piece, how to write it, what a good boot looks like.
- **[docs/rknand-re/](docs/rknand-re/)** — the reverse engineering: NFC register
  map, BCH ECC scheme, the data-randomizer seed table, OOB format, L2P algorithm.
- **[docs/hardware.md](docs/hardware.md)** — board, pinout, serial console.

## Hardware

| | |
|---|---|
| Board | `rk322x-box` (Generic RK322x TV Box), Armbian `LINUXFAMILY=rockchip` |
| SoC | Rockchip RK3229 (Cortex-A7 ×4, armv7l), 2 GB DDR3, ARM clock 1.2 GHz |
| NAND | Micron MT29F64G08CBABAWP, 8 GB MLC — page 8192 B, OOB 744 B, erase 2 MiB |
| NAND controller | `nand-controller@30030000`, Rockchip NFC v6/v8 |
| WiFi | Realtek RTL8189ES, SDIO `024C:8179` on `mmc1` (non-removable) |
| Console | UART2, `ttyS2`, **115200 8N1** |

## Layout

```
docs/                             hardware, boot chain, RE notes, write-up series
docs/journey/                     how it was done, pitfalls, lessons
docs/rknand-re/                   reverse engineering of the vendor FTL
drivers/wifi-rtl8189es/           Track A — RTL8189ES module build
drivers/nand-rk322x/rknand-port/  vendor rk_nand FTL driver forward-ported to 6.6
drivers/nand-rk322x/overlays/     device-tree overlays (mainline MTD / vendor FTL)
drivers/nand-rk322x/uboot/        patches for the vendor U-Boot 2017.09
tools/                            mk-idblock.py, mk-rkparam.py, diagnostics
env/                              reproducible cross-build (Docker), pinned to 6.6.22
extracted/                        (gitignored) blobs carved from the vendor 4.4 ROM
```

## Recovery

Nothing here is a one-way door, provided you keep one thing in mind: **once a
valid idblock is on NAND the BootROM boots NAND and ignores the SD card**. To get
back to an SD boot, **short NAND pins 29+30 during power-on** — the BootROM then
fails to read the flash and falls through to the card. The board also has a
reset/recovery button, and a serial console on `ttyS2` at 115200.

## License

Kernel and U-Boot changes: **GPL-2.0**. The Rockchip FTL blobs are vendor-shipped
assembly redistributed from the public BSP; nothing under `extracted/` (carved
from the vendor ROM) is redistributed. See [`LICENSE`](LICENSE).
