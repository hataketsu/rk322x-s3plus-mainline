# Step 3 — FTL L2P reconstruction (roadmap)

With steps 1 (descramble) and 2 (40-bit ECC) done, mainline reads the vendor rknand NAND
**physical** pages losslessly. Step 3 turns physical reads into a **logical** view (the
rootfs the FTL presents as a block device) so it can be mounted / booted.

## What the FTL does

`rknand` is a page-mapped FTL: the block layer sees logical sectors; the FTL maps each
logical page to a physical page via mapping tables kept in "system"/"map" blocks on the
NAND, plus superblock grouping across planes/dies. Reading the rootfs in order needs the
**logical→physical (L2P) map**.

## Two routes

### A. Scan-based (empirical, likely tractable)
Each physical page's OOB **sys data** (4 bytes/step, 32 B/page that mainline already
exposes) appears to carry per-page FTL metadata. A raw OOB dump of a data page shows
non-zero bookkeeping in the first step's sys bytes, e.g.:

```
page @64MiB, OOB: ff ef 6f 03  46 f1 07 00  57 84 03 00  00 00 ...
                  \__ ~0x036fef, 0x07f146, 0x038457 — LPA / seq / version? __/
```

If the first sys word encodes the page's **logical address (LPA)** + a version/sequence,
then L2P can be rebuilt WITHOUT reimplementing the FTL:
1. Scan every physical page, read its OOB sys bytes.
2. Decode `(LPA, version)`; keep the newest version per LPA (handles GC/rewrite).
3. Invert → logical→physical; emit a logical image; loop-mount to verify (ext4).

Next task: decode the exact sys-byte layout from the write path — `ftl_prog_page` →
`func_0xb0967138` (page program) and `ftl_info_*` — to learn which bytes are LPA vs
version vs block-type. Cross-check by scanning and testing whether the reconstructed
logical image mounts.

### B. Full FTL map-table parse (authoritative, more work)
Reimplement the vendor map load: `FtlScanSysBlk` (find system blocks) → `FtlLoadSysInfo` /
`FtlLoadMapInfo` / `FtlLoadBbt` (load L2P + BBT from flash) → `ftl_get_ppa_from_index`
(the per-superblock LPA-table lookup already decompiled). This is the correct path if the
OOB doesn't fully encode L2P (e.g. map lives only in dedicated map blocks). ~90 decompiled
`ftl_*` functions are the reference.

## Key decompiled functions (in extracted/kernel-4.4/decompiled/, gitignored)
`ftl_get_ppa_from_index`, `FtlLoadMapInfo`, `FtlLoadSysInfo`, `FtlScanSysBlk`,
`ftl_info_blk_init`, `ftl_info_flush`, `ftl_prog_page`, `ftl_read_ppa_page`,
`FtlLoadBbt`, `FtlMakeBbt`, `ftl_update_l2p_map`.

## After step 3
- **4. 6.x kernel block driver** exposing the logical view as `/dev/rknand0` (or an MTD
  translation layer) so the rootfs mounts.
- **5. Boot**: vendor U-Boot 2017.09 already reads rknand and loads a kernel; with a 6.x
  kernel that can mount the NAND rootfs, the box boots current from NAND. (Alternatively a
  fully in-kernel path; u-boot side stays vendor.)

## Effort
Step 3 is the large piece (multi-session). Route A is the fastest thing to try next and is
verifiable end-to-end (does the reconstructed image mount?). Everything up to here —
lossless physical reads — is done and in the repo.
