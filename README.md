# Kiwibox S3 Plus (RK322x) — NAND boot & WiFi driver port to mainline Armbian

Bringing the **onboard NAND** and **RTL8189FS WiFi** of a Rockchip RK322x TV box
(Kiwibox S3 Plus, SoC RK3229) back to life on the **current Armbian kernel (6.6.x)**,
where the vendor's closed 4.4 drivers no longer apply.

> Status: **early** — environment + reverse-engineering scaffold. WiFi track is close
> to a working module; NAND track is a research/porting effort. See the status matrix.

## Why this exists

The vendor only ever shipped working NAND + WiFi on the **legacy 4.4 kernel** (a closed
`rknand` blob + an out-of-tree `8189fs` driver). Armbian's **current 6.6 image boots
fine from SD card but:**

- **no `wlan0`** — RTL8189FS (SDIO `024C:8179`) has no in-tree SDIO driver, and
- **cannot boot from / use onboard NAND** — the `nand-controller@30030000` has no
  mainline MTD driver bound (`/proc/mtd` empty, no `/dev/mtd*`).

This repo reverse-engineers the vendor 4.4 ROM and the Multitool boot chain to close
both gaps on 6.6.

## Hardware (this unit)

| | |
|---|---|
| Board | `rk322x-box` (Generic RK322x TV Box), Armbian `LINUXFAMILY=rockchip` |
| SoC | Rockchip RK3229 (Cortex-A7 ×4, armv7l), 2 GB DDR3 |
| WiFi | Realtek **RTL8189FS**, SDIO id `024C:8179` on `mmc1` (non-removable) |
| NAND | Rockchip NAND, controller `nand-controller@30030000`, `rknand` v1.2 under 4.4 |
| Console | UART2, `ttyS2` **115200 8N1** |
| Running now | Armbian 24.2.5, kernel `6.6.22-current-rockchip`, booted from SD |

Full findings + pinouts: [`docs/hardware.md`](docs/hardware.md).

## Status matrix

| Track | Goal | Feasibility | State |
|---|---|---|---|
| **A — WiFi** | RTL8189ES on 6.6 | **Feasible** — out-of-tree source builds | **✅ WORKING** — `wlan0`, WPA2, DHCP, internet |
| **B — NAND** | Boot/use NAND on 6.6 | **Hard RE** — vendor `rknand.ko` is a closed, ABI-locked blob; needs a mainline MTD driver written for the rk322x NAND controller | research |

**Track A is done:** `8189es.ko` cross-built against a source KDIR pinned to the box's
exact vermagic (`6.6.22-current-rockchip`), loads on the running 6.6 kernel, associates
to WPA2, gets DHCP + IPv6, reaches the internet — and auto-loads on boot. Recipe in
[`drivers/wifi-rtl8189es/`](drivers/wifi-rtl8189es/).

**The honest constraint for Track B:** the 4.4 `rknand.ko` binary *cannot* be recompiled
for 6.6 (no source, kernel ABI changed massively 4.4→6.6). The realistic path is to
**write a mainline MTD driver** for the `nand-controller@30030000`, using the vendor
blob's register access (via disassembly) and the Multitool steP-nand chain as reference.
See [`docs/nand-boot.md`](docs/nand-boot.md).

## Layout

```
docs/                 hardware notes, NAND boot chain, RE methodology
env/                  reproducible cross-build (Docker) — pinned to kernel 6.6.22
scripts/              ROM extraction, SD flashing, box SSH helper
drivers/wifi-8189fs/  Track A — RTL8189FS module build
drivers/nand-rk322x/  Track B — mainline NAND MTD driver (WIP)
extracted/            (gitignored) blobs carved from vendor 4.4 ROM
images/               (gitignored) Armbian .img.xz — fetch from mirror
```

## Quick start

```bash
# 1. bring up the pinned cross-build toolchain (kernel 6.6.22 headers)
make -C env toolchain

# 2. carve the vendor 4.4 ROM for reference blobs (rknand.ko, 8189fs firmware)
./scripts/extract-rom.sh images/Armbian_21.05.1_Rk322x-box_buster_legacy_4.4.194.img.xz

# 3. build the WiFi module
make -C drivers/wifi-8189fs
```

## License

Kernel drivers: **GPL-2.0**. Vendor blobs under `extracted/` are **not** redistributed.
See [`LICENSE`](LICENSE).
