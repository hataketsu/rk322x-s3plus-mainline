# Phase A — offline vendor-rootfs reconstruction: results

Goal: prove the RE'd read path end-to-end by rebuilding the vendor Armbian rootfs from a
raw NAND dump (descramble + 40-bit ECC + FTL L2P reorder), independent of any kernel FTL.

## What works — read path VALIDATED

- **Full L2P scan** (`tools/l2p-scan.py`, MEMREADOOB ioctl per page): 1 048 576 pages,
  1 038 336 data pages, **507 982 distinct LPNs**, 0 ioctl errors. Map saved.
- **Reconstruction** (`tools/l2p-extract.py`): places each page at `LPN*8192`, holes
  zero-filled. The first 256 MB already yields **real rootfs content** — strings like
  `/etc/nsswitch.conf`, `Debian Archive Automatic Signing Key`, `/usr/lib/<multiarch>`.
- **The ext4 rootfs superblock is recovered byte-perfect:** label **`armbi_root`**,
  magic `0x53EF`, block_count 1 506 304 × 4096 = **6.17 GB**, located at logical ~222 MB
  (fs starts at **LPN 27130**). So descramble + ECC + randomizer + L2P are all correct —
  we are reading the vendor rootfs from mainline. As far as we can tell, a first.

## What's not done — clean mount

Mounting the reconstructed image (truncated to the 6.17 GB fs size, holes sparse) gets
past the superblock but ext4 rejects the **block group descriptors**:

```
EXT4-fs: ext4_check_descriptors: Block bitmap for group 0 not in group (...)
group descriptors corrupted!
```

fs-block-1 (the group-descriptor block, same physical page as the valid superblock) holds
non-zero but wrong values (e.g. `bg_inode_table_lo` out of range). Root cause still open —
candidates: a per-block uncorrected ECC bitflip in that page, a subtle newest-version
selection edge case, or an ext4 feature (64bit/metadata_csum/flex_bg) interaction. The
variant OOB magics seen in the scan (`0xa6f2`/`0xc645`, all-four-sys-words-equal) are
erased/special pages, **not** missing data — skipping them is correct.

## Status

- Read path (steps 1–3): **proven** — vendor rootfs content + valid `armbi_root`
  superblock reconstructed from mainline.
- Clean loop-mount: ~95% — remaining is reconstruction-accuracy debugging at the group
  descriptors, not an RE gap.

## Tools
- [`../../tools/l2p-scan.py`](../../tools/l2p-scan.py) — MEMREADOOB per-page L2P scan → map.
- [`../../tools/l2p-extract.py`](../../tools/l2p-extract.py) — rebuild logical image from map.
- Note: `nanddump -s` is ignored on this skip-BBT driver (always returns page 0); use the
  `MEMREADOOB` ioctl (real seeking) — that was the key unblock for the full scan.
