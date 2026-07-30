# Phase A — the plane-interleave / superpage scramble (read-accuracy gap)

Follow-up to `phase-a-results.md`. The per-page read path is byte-exact (controller +
40-bit BCH + randomizer + OOB de-rotation all verified — dumpe2fs 1.47 parses the primary
ext4 superblock perfectly). The remaining gap is **logical placement**: reconstructing the
full logical image as `page(LPN) @ LPN*8192` scrambles the ext4 block order, so the group
descriptor block comes out wrong and the fs won't mount.

## Evidence

Full-device OOB audit (1 048 576 pages, 0 ioctl errors):

- `data pages 1 038 336`, **distinct LPNs 507 982**, **duplicate LPNs 530 354** — every LPN
  appears ~2×.
- The two copies of an LPN sit **exactly `device/2` = 524 288 pages apart**, and carry
  **identical OOB** (magic 0xF095, same version, same LPN, same prevPPA — all 8 sys-words
  equal). OOB cannot disambiguate them.
- One copy is real, the other garbage. For LPN 27130: phys **790260** = perfect ext4
  superblock; phys **265972** = noise (random magic 0xbf4b). The real copy is the **high
  half** here; l2p.map's "last-wins" tie-break happens to keep the high-half page.

## The real mapping is 2-plane, per-parity linear

Sampling the real (high-half) copies around the superblock:

```
even LPN L -> phys 790260 + (L-27130)/2      # 27130->790260, 27132->790261, 27148->790269
odd  LPN L -> phys 790515 + (L-27129)/2      # 27129->790515, 27131->790516, 27133->790517
```

So plane = `LPN & 1`; consecutive LPNs alternate between two physical page-streams. This is
consistent with the vendor PPA geometry (`page = ppa & 0x3FF` = 1024 pages/block) vs the
mainline erase block (2 MB / 8192 = 256 pages): one vendor "superblock" = 4 mainline
erase-blocks = multi-plane.

## The unresolved stride

Locating the real group-descriptor block (flex_bg signature: descriptor `bg_block_bitmap`
= 737, 738, 739 at 64-byte stride):

```
GDT content found at OOB.LPN 27133 (off 4096), 27143 (off 4096), 27148 (off 0)
```

The ext4 primary gdt is fs **block 1** (immediately after the block-0 superblock). The
superblock is at LPN 27130; the gdt is at **LPN 27133**, not LPN 27130's second half. So
**ext4 block N does not map to LPN `27130 + N/2`** — there is an additional vendor superpage
stride between the ext4 byte offset and the OOB.LPN that we have not yet reversed.

## Consequence & status

- Reading any single page: **byte-exact / proven.**
- Reconstructing the whole logical image: **blocked** on the superpage stride above. The
  2-plane parity split is solved; the ext4-block↔LPN stride is not.
- The vendor `armbi_root` fs is **nearly empty** (dumpe2fs: 11 inodes used, `Lifetime
  writes: 2973 kB`, created 2024-03-25, mounted once) — so preserving its contents has low
  value. The interleave must still be reversed because the **write path needs the same
  physical↔logical mapping**.

## RESOLUTION (2026-07-24): read is byte-faithful; the source is an aborted install

Further RE closed this out:

- **nplanes = 2, confirmed two independent ways.** (a) even/odd LPN map to two physical
  page-streams; (b) the vendor's own authoritative map page (Route B, magic 0xF0C2 at OOB
  sys-word 2) maps consecutive logical indices to **alternating physical blocks 1022/1023**
  with the page number incrementing — i.e. `plane = idx & 1`, `page = idx >> 1`, one region
  (2048 LPNs) = one 2-block superblock. `ftl_get_ppa_from_index` matches: `PPA =
  superblk.block[plane]*pages_per_block + page`.
- **OOB magic is at sys-word 2**, not word 7. Word 7 (`0x03d9f095`) is a constant footer on
  *every* page (data and sys), so the earlier "1 038 336 data pages" over-counted — sys/map
  pages share the footer. The real per-page magic (data / 0xF0A4 sysinfo / 0xF0C2 map /
  0xF086 ext) lives in word 2.
- **Route B has only 2 map pages** (the fs is nearly empty), and neither covers region 13
  (LPN 27130). So LPN 27130's data lives in an **open superblock**, recoverable only by the
  per-page OOB scan (Route A) — which is exactly what `l2p-scan.py` does. Route A is the
  correct and sufficient method for this device; there is no richer authoritative map.
- **The gdt is unrecoverable because it was never validly written.** The correct physical
  copy of LPN 27130 (phys 790260, proven by the byte-perfect superblock) contains a good
  superblock and a non-gdt second half; the other copy (265972) is pure noise; no version
  carries a valid gdt. dumpe2fs 1.47 parses the primary superblock perfectly and 0 ECC
  errors occur device-wide — so we are reading exactly what is on the media.

**Conclusion:** the per-page **read path is byte-perfect / proven**. The vendor `armbi_root`
is a nearly-empty *aborted* install (11 inodes, 2973 kB lifetime writes, mkfs 2024-03-25,
mounted once) whose primary group-descriptor block was never validly programmed — so there
is no complete mountable filesystem on the NAND to recover. The mount fails on the source,
not on our reconstruction. Read verification is complete.

## Basis for the write path

The 2-plane model is now known well enough to build writes: a region/superblock = 2
physical blocks, `plane = logicalindex & 1`, sequential page program within each block, OOB
struct with magic at word 2 + the constant word-7 footer, randomizer seed `table[page&0x7f]
| 0xc0000000`, 40-bit BCH. Validate with a write→readback round-trip on a verified-free
superblock (box boots from SD independently, so a bad NAND write cannot brick the box — only
a hard kernel hang would need a physical power-cycle).
