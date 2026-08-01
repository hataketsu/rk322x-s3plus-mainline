# 01 — Reading a flash nobody can read

The first surprise was a pleasant one: this box's NAND controller is **not**
proprietary. The device-tree node `nand-controller@30030000` is

```
compatible = "rockchip,rk3228-nfc", "rockchip,rk2928-nfc";
```

and mainline's `drivers/mtd/nand/raw/rockchip-nand-controller.c` already matches
`rockchip,rk2928-nfc` (as `nfc_v6`). So no controller driver had to be written.
Two config problems stood in the way instead:

- Armbian's rk322x kernel ships `# CONFIG_MTD is not set`, so the whole MTD stack
  is absent. Built out-of-tree against a source KDIR pinned to the box's exact
  vermagic. `lib/bch.c` needs a `MODULE_LICENSE` line added to build as a module.
- The DT node is `status = "disabled"`. Enabled at runtime with a configfs
  overlay, no reboot needed.

With that, the chip announced itself:

```
nand: device found, Manufacturer ID: 0x2c, Chip ID: 0x64
nand: Micron MT29F64G08CBABAWP
nand: 8192 MiB, MLC, erase size: 2048 KiB, page size: 8192, OOB size: 744
```

And every page read back as noise.

## Why the data looked like noise

Three separate things stand between a working controller and readable vendor
data. Each had to be recovered from the vendor 4.4 kernel (extracted from the
ROM, disassembled with Ghidra against the shipped `System.map`).

### 1. The hardware randomizer

Rockchip scrambles data on the way to the flash with a hardware randomizer seeded
per page. Mainline's driver knows the register — `RANDMZ_CFG` at `0x150` for
v6/v8, `0x208` for v9 — and deliberately **disables** it, because upstream has no
reason to reproduce a vendor scrambling scheme.

The seed is not a formula but a table: **128 × u16, indexed by `page & 0x7f`**,
OR'd with `0xc0000000` to enable. Recovered from the vendor kernel and published
as [`randomizer-seed-table.txt`](../rknand-re/randomizer-seed-table.txt). Program
it before each non-boot page access and the noise turns into data — the first
proof was recovering real filesystem strings (LibreOffice paths, ZIP headers) out
of pages that were pure entropy a moment earlier.

Boot blocks are different: the randomizer is **off** for them, which is why
blocks 0–7 read as clean `0xFF` when erased while everything above reads as the
descrambled-blank pattern `2c99d86e7138bdde`. That difference turns out to be a
useful diagnostic later.

### 2. The ECC strength is 40, not 60

The vendor U-Boot prints `ECC:60` at boot, and that number is a trap. It is
`g_idb_ecc_bits` — the strength used for the **idblock only**. The data area uses
`g_nandc_ecc_bits = 40`, taken from the chip's entry in the vendor parameter table
(for this Micron part the ECC byte is `0x28` = 40).

Chasing 60 wastes time in a specific way: 60-bit BCH needs 840 B of parity, which
does not fit in this chip's 744 B OOB, so the driver simply refuses to probe with
`-EINVAL`. At 40 bits the geometry is

```
8 steps × [ 1024 B data | 4 B sys | 70 B parity ] = 592 B OOB   (≤ 744)
```

which is exactly mainline's native `8 × 1024 / 40-bit` layout. No ECC code
changes were needed — only `nand-ecc-strength = <40>` and
`nand-ecc-step-size = <1024>` in the DT.

### 3. Every block looks bad

Mainline checks the factory bad-block marker at a fixed offset in the OOB of each
block's first page. The vendor stores **FTL metadata** at that offset, so almost
every block reads as "bad" and erase is refused. Two changes fix it:
`NAND_SKIP_BBTSCAN`, and a `chip->legacy.block_bad` stub returning 0, installed
before `nand_scan`.

With all three in place: **0 ECC errors device-wide, byte-perfect data, 16.6 MB/s**.

## The OOB metadata

Reading pages is not the same as knowing which page holds which data. The FTL's
logical-to-physical map is reconstructible because every page carries its own
metadata in the NFC "sys" bytes — 4 bytes per ECC step, so 16 bytes across steps
0–3:

| offset | field |
|---|---|
| +0x00 | `u16` magic — data `0xF095`, sysinfo `0xF0A4`, map `0xF0C2`, ext `0xF086`, erased `0xFFFF` |
| +0x02 | tag |
| +0x04 | `u32` version — newest wins, with wrap-around comparison |
| +0x08 | `u32` LPN — logical page number |
| +0x0C | `u32` prevPPA |

A PPA decodes as `blk = (ppa & 0x3FFFFFF) >> 10`, `page = ppa & 0x3FF`, and bit 31
means the page is in pseudo-SLC mode.

Two traps here, both of which cost real time:

- **Mainline exposes the sys bytes rotated.** Step 0 lands at OOB offset
  `(steps-1) * 4 = 28`, and steps 1–7 land at offsets 0–24. Read them in the
  naive order and every field is garbage.
- **The magic is at sys-word 2, not word 7.** Word 7 is a constant footer
  (`0x03d9f095`) present on *every* page, data and system alike. An early audit
  keyed off word 7 and consequently counted system pages as data pages.

## Seeking is a lie

`nanddump -s <offset>` silently ignores the offset on a driver with
`NAND_SKIP_BBTSCAN` and returns page 0 every time. Every "scan" built on it
produces a beautiful, entirely fictional map.

The way to actually seek is the **`MEMREADOOB` ioctl** (`0xC00C4D04`), which takes
a real byte offset and, as a bonus, returns descrambled OOB. `os.pread()` works
for the data area. Both are used by [`tools/l2p-scan.py`](../../tools/) and
`l2p-extract.py`.

## Two planes, not one

The final structural surprise: scanning all 1048576 pages found 507982 distinct
LPNs but 530354 *duplicate* LPNs — every LPN appearing about twice, the two copies
exactly `device/2` pages apart, with **identical OOB** (same magic, version, LPN,
prevPPA — nothing in the metadata distinguishes them). One copy is real, the other
is noise.

That is a 2-plane interleave: `plane = LPN & 1`, a region of 2048 LPNs maps to one
2-block superblock, and `PPA = block[plane] * pages_per_block + page`. It was
confirmed twice over — once from the even/odd LPN → two physical streams pattern,
and once from the vendor's own map pages (magic `0xF0C2`), which map consecutive
indices to alternating physical blocks 1022/1023.

The read path was declared complete when a reconstructed image produced the
`armbi_root` ext4 superblock **byte-perfect** (`dumpe2fs` parses it cleanly,
magic `0x53EF`, 6.17 GB). The filesystem still would not mount — but that turned
out to be because the vendor's own install was an aborted `mkfs` whose primary
group descriptors were never written, not a gap in the reverse engineering.

Full detail: [`../rknand-re/`](../rknand-re/) — in particular
[`ftl-oob-format.md`](../rknand-re/ftl-oob-format.md),
[`ftl-l2p-algorithm.md`](../rknand-re/ftl-l2p-algorithm.md), and
[`phase-a-plane-interleave.md`](../rknand-re/phase-a-plane-interleave.md).

## What this bought, and what it did not

At the end of this phase mainline could **read** vendor NAND losslessly. It could
not write it safely, and reimplementing the FTL's write path — wear levelling,
GC, bad-block management, power-fail safety — was a multi-week project with a lot
of ways to quietly corrupt a flash.

So the read work became a *verification tool* rather than the product. The actual
path forward was to port the vendor FTL itself, which is
[article 02](02-porting-the-ftl.md). Everything above kept its value: it is how
we could later inspect the media independently of the FTL and settle arguments
about what was really on the flash — which is exactly what cracked
[article 03](03-the-write-protect-bug.md).
