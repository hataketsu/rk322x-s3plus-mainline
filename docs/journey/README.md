# How this port was done

A write-up series, in the order the problems actually appeared. It is meant for
two audiences: someone reproducing this on the same box, and someone porting to a
different RK322x board who wants to know which walls are real.

The procedure itself lives in [`../nand-boot.md`](../nand-boot.md); these articles
are the reasoning, the dead ends, and the measurements.

| | |
|---|---|
| [01 — Reading a flash nobody can read](01-reading-the-vendor-nand.md) | Why mainline sees vendor NAND as noise, and the three things (randomizer, ECC geometry, OOB) that had to be recovered before anything else could work. |
| [02 — Porting the closed FTL to kernel 6.6](02-porting-the-ftl.md) | Taking a driver written for 5.10 and a blob written for nothing in particular, and making both run on 6.6. |
| [03 — The bug that ate three days](03-the-write-protect-bug.md) | Writes that reported success and changed nothing. The most instructive failure in the project, and how measurement finally beat theorising. |
| [04 — Getting the vendor U-Boot to boot us](04-uboot.md) | Four separate defects in Rockchip's 2017.09 U-Boot, and why the fix is a config override rather than a patch. |
| [05 — Pitfalls and lessons](05-pitfalls-and-lessons.md) | The condensed checklist. If you only read one file, read this one. |
| [06 — References](06-references.md) | Sources, tools, and what was actually load-bearing. |

## The short version

The box has an 8 GB MLC NAND managed by Rockchip's closed FTL. Mainline Linux can
drive the NAND *controller* — `rockchip-nand-controller.c` matches
`rockchip,rk2928-nfc` — but not the *format*: the data is scrambled by a hardware
randomizer, protected by 40-bit BCH in a specific geometry, and laid out by a
logical-to-physical map that lives in the OOB area. All three were reverse
engineered from the vendor 4.4 kernel, which is enough to *read* the flash from
mainline.

Reading is not booting. For that the vendor FTL itself was forward-ported to
kernel 6.6 as `rknand.ko`, which gives `/dev/rknand0` — a normal block device with
wear levelling and bad-block management. The same FTL blob is already linked into
the vendor U-Boot, so U-Boot and Linux see identical partitions, and the boot
chain becomes ordinary: extlinux on an ext4 partition.

What made it hard was not the reverse engineering. It was a single missing entry
in a device-tree property — `flash-wp` — which left the NAND write-protected while
every register said otherwise, and which produced a failure that looked exactly
like a broken FTL for days. That story is [article 03](03-the-write-protect-bug.md).
