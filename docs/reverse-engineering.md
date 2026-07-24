# Reverse-engineering methodology

What we extract from the vendor artifacts, and how. Everything carved lands under
`extracted/` (gitignored — vendor blobs are not redistributed).

## Sources

| Artifact | Contains | How to get |
|---|---|---|
| Armbian 4.4 legacy `.img.xz` | `rknand.ko`, `8189fs.ko`, WiFi firmware, 4.4 `.dtb`, u-boot | `images/` (in repo `.gitignore`) |
| Multitool `.xz` | steP-nand scripts, `bsp/*.img` loaders, exact NAND dd offsets | `apt.undo.it:7243/multitool-rk322x.xz` |
| Running box (6.6) | live DT, SDIO ids, dmesg, NAND controller node | `scripts/rsh.sh` |

## Extracting the ext4 rootfs without mounting (macOS host)

macOS can't mount ext4. `scripts/extract-rom.sh` runs `debugfs` (e2fsprogs) inside a
throwaway Debian container to `rdump` paths out of the image's rootfs partition
(ext4 at 4 MiB offset). No root, no loopback on the host.

Pulled by default:
- `/lib/modules/<4.4>/…/rknand.ko`, `8189fs.ko` (+ `modinfo`)
- `/lib/firmware/rtlwifi/`, `/lib/firmware/rtl_bt/`
- `/boot/*.dtb`, `/boot/dtb/…` (4.4 device tree for the NAND node)
- `/usr/bin`, `/usr/lib` steP-nand helpers if present

## Disassembling `rknand.ko` (Track B core)

```bash
arm-linux-gnueabihf-objdump -d -j .text extracted/rknand.ko > extracted/rknand.asm
arm-linux-gnueabihf-nm      extracted/rknand.ko             # symbols the blob exports/needs
modinfo                     extracted/rknand.ko             # vermagic, params
```

Goal: recover the MMIO access pattern at base `0x30030000` —
- command / address / data register offsets,
- ECC engine config + strength,
- page size / OOB size / timing,
- vendor-block & BBT handling.

Cross-reference against mainline `drivers/mtd/nand/raw/rockchip-nand-controller.c` to
decide: *supported NFC variant* (→ DT compatible + bindings) vs *new driver needed*.

## DT diff

Compare the 4.4 `nand-controller`/`nandc` node (from the extracted `.dtb`, decompiled with
`dtc -I dtb -O dts`) against the 6.6 live node
(`/proc/device-tree/nand-controller@30030000`) to see what properties the mainline driver
would need (clocks, ECC, `nand-ecc-*`, partitions).

## Safety

- Vendor blobs stay in `extracted/` (gitignored). Do **not** commit or redistribute.
- Only GPL / clean-room-derived code goes into `drivers/`.
- Register maps recovered from disassembly are facts about hardware, documented in
  `nand-boot.md`; the *implementation* is written fresh against mainline MTD APIs.
