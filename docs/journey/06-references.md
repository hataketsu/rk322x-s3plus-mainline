# 06 — References

What was actually load-bearing, and where it came from.

## Primary sources

**The vendor 4.4 kernel from this box's own ROM.** The single most valuable
artefact. Extracted from the legacy Armbian 4.4 image (`zImage`, gzip payload at
offset `0x47b8`), disassembled with Ghidra headless against the shipped
`System.map` (101k symbols, `_text = 0xb0008000`, so `file_off = VA - _text`).
This is where the randomizer seed table, the ECC geometry, the OOB format and the
`write_idblock` behaviour came from. Kept under `extracted/` and **not**
redistributed.

**The Rockchip 5.10 BSP `drivers/rk_nand/`.** The FTL blobs plus ~1200 lines of
glue that became [`drivers/nand-rk322x/rknand-port/`](../../drivers/nand-rk322x/rknand-port/).
Obtained via a Luckfox SDK checkout. Note there are three plausible-looking trees
and only this one is right — see [article 02](02-porting-the-ftl.md).

**Rockchip U-Boot 2017.09** (`rk322x_defconfig`, `TARGET_EVB_RK3229`), from the
same SDK. Everything cited in [article 04](04-uboot.md) is from reading this tree:
`disk/part_rkparm.c`, `arch/arm/mach-rockchip/param.c`, `cmd/rknand.c`,
`cmd/pxe.c`, `include/configs/rockchip-common.h`, `common/board_f.c`.

**`rkbin`** — `rk322x_ddr_300MHz_v1.10.bin`, `rk322x_miniloader_v2.56.bin`,
`rk322x_tee_v2.00.bin`, plus `boot_merger` and `RK322XMINIALL.ini`.

**Mainline `drivers/mtd/nand/raw/rockchip-nand-controller.c`.** Doubles as
documentation for the NFC register map and as the independent read/write path that
cracked [article 03](03-the-write-protect-bug.md). Its `nfc_v6_cfg` /`nfc_v8_cfg`
structs are the authoritative offset list.

**Mainline U-Boot `tools/rkcommon.c`.** The best documentation of the idblock
header — `struct header0_info`, `RK_MAGIC = 0x0ff0aa55`, the RC4 key, and the fact
that the header is encrypted while the payload need not be.

## Hardware documentation

- Micron **MT29F64G08CBABAWP** datasheet — page/OOB geometry, the ONFI behaviour
  of WP#, and the guaranteed-valid-block count (≥ 4016 of 4096, i.e. up to 80 bad
  blocks are normal).
- ONFI: with WP# low, PROGRAM and ERASE are ignored. Exactly the symptom in
  article 03.
- Rockchip RK3229 TRM for the GRF IOMUX layout — `GPIO2B_IOMUX` at `0x11000024`,
  2 bits per pin, write-enable in the upper 16 bits.

## Tools

| tool | used for |
|---|---|
| Ghidra (headless) | disassembling the vendor 4.4 kernel |
| `dtc` / `fdtoverlay` | building and merging device-tree overlays |
| `mkimage` (`tools/` in the U-Boot tree) | inspecting RK image formats |
| `mkinitramfs -d <confdir>` | building an initrd without touching the running system's config |
| `systemd-analyze critical-chain` | the only boot-time measurement that predicts anything |
| `ethtool`, `/sys/kernel/debug/pinctrl`, `/sys/kernel/debug/clk/clk_summary` | confirming what the hardware is actually doing |
| `/dev/mem` + Python `mmap` | reading and poking GRF/NFC registers at runtime |

## This repo

| | |
|---|---|
| [`tools/mk-idblock.py`](../../tools/mk-idblock.py) | build/inspect a BootROM idblock from an rkbin BOOT container |
| [`tools/mk-rkparam.py`](../../tools/mk-rkparam.py) | build/inspect the RK `parameter` partition table |
| [`tools/nand-cellstate.py`](../../tools/nand-cellstate.py) | classify blocks as erased / programmed via the mainline driver — the article-03 discriminator |
| [`tools/nfc-regdump.py`](../../tools/nfc-regdump.py) | dump NFC registers around a known-good write |
| [`tools/uart-log.sh`](../../tools/uart-log.sh) | serial capture with auto-reconnect |
| [`docs/rknand-re/`](../rknand-re/) | the reverse-engineering notes and verification samples |
| [`drivers/nand-rk322x/uboot/`](../../drivers/nand-rk322x/uboot/) | patches against vendor U-Boot 2017.09 |

## Prior art and context

- Armbian's rk322x support and its maintainer's position that 6.x is SD-only on
  these boxes. That was accurate for the tooling available at the time; the gap
  was the FTL, and porting it rather than reimplementing it is what closed it.
- Multitool's steP-nand installer, the reference for how vendor installs lay out
  the flash.
- The RTL8189ES out-of-tree driver for Track A, built against a KDIR pinned to the
  box's exact vermagic — see
  [`drivers/wifi-rtl8189es/`](../../drivers/wifi-rtl8189es/).

## Things that were not needed

Worth listing, because each looked necessary at some point:

- **USB / maskrom.** Never used. The idblock is written from Linux through the
  FTL's `write_loader_lba` hook.
- **A mainline FTL reimplementation.** The read path was reverse engineered and
  proven, but porting the vendor FTL was faster and safer than reimplementing wear
  levelling and GC.
- **A newer U-Boot.** Portable in principle — the glue is only ~300 lines
  (`rknand.c` + `rknand.h`) against U-Boot's DM — but it buys only USB storage,
  netboot and a working `distro_bootcmd`, none of which block anything.
- **UBI/UBIFS on raw MTD.** The alternative architecture. Viable, but it means
  abandoning the vendor FTL entirely, and MLC needs pseudo-SLC handling that UBI
  does not provide.
