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

---

## IMPORTANT alternative — the "fresh mainline" path (avoids FTL RE entirely)

Everything above (steps 1–3) is about **reading the EXISTING vendor data**. If the goal is
just *"6.x boots from NAND"* and **wiping the NAND is acceptable** (owner said a clean
reinstall is fine), there is a lighter, more-mainline route that skips the FTL reimplement:

```
block 0..N   : Rockchip vendor idbloader + u-boot   (BootROM-format — keep/rewrite as-is)
rest of NAND : mainline MTD + UBI/UBIFS              (fresh — no vendor FTL, no RE)
```

- **Kernel side is essentially solved:** we already proved mainline `rockchip-nand-controller`
  drives this NFC. On a *fresh* erase there is no vendor scramble/ECC to match — the driver
  writes and reads with its own consistent randomizer/ECC. So a mainline kernel with
  `CONFIG_MTD + MTD_RAW_NAND + MTD_NAND_ROCKCHIP + UBI/UBIFS` + the DT node enabled can host
  the rootfs on NAND. (No FTL, no descramble, no L2P RE needed for fresh data.)
- **The real remaining gap is U-Boot**, not the FTL:
  - Vendor U-Boot 2017.09 reads rknand FTL but **not** MTD/UBI → can't load a kernel from a
    UBI partition.
  - Mainline U-Boot reads UBI but **rk322x NAND (NFC) support in mainline U-Boot is
    weak/absent** → can't drive this controller to read raw NAND.
  - ⇒ The task becomes **port the NFC driver into U-Boot for rk322x** (raw NAND + UBI read),
    a bounded MTD-in-U-Boot job — much smaller than reimplementing the closed FTL.

**Recommendation:** for a *booting* box on 6.x, the fresh MTD+UBI path (kernel proven +
U-Boot NFC port) is likely faster than the FTL-RE path. The FTL RE (steps 1–3) is still
worthwhile for *reading existing vendor data* and as documentation, but is not on the
critical path to a fresh NAND boot. **Decide with the owner which goal matters: keep
existing data (→ FTL RE) vs just boot 6.x from NAND (→ fresh MTD+UBI + U-Boot port).**

> Not done autonomously: erasing/writing the NAND is destructive and irreversible — hold
> for explicit owner go-ahead before any wipe.
