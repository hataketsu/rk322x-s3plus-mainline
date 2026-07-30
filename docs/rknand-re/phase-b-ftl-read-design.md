# Phase B — in-kernel rknand read-only FTL (design)

Reimplement the vendor rknand logical→physical (L2P) translation in a mainline
6.6.x module so the vendor NAND's logical rootfs is exposed as a **read-only**
block device. This FTL sits **above** the existing mainline
`rockchip-nand-controller.c` (which already does NAND_SKIP_BBTSCAN + randomizer
descramble and gives us raw descrambled 8192-byte pages + rotated 16-byte OOB
through the MTD `read`/`read_oob` interface). Write/GC is a later phase and is
**not** implemented here.

Authoritative algorithm sources (do not re-derive): `ftl-l2p-algorithm.md`,
`ftl-oob-format.md`, `nand-info-struct.md`, and the working Python read path
`tools/l2p-scan.py` + `tools/l2p-extract.py`.

Deliverables (this area only):
- `drivers/nand-rk322x/ftl/rk-ftl.h`
- `drivers/nand-rk322x/ftl/rk-ftl.c`
- `drivers/nand-rk322x/ftl/Makefile`
- this doc

---

## 1. Chosen block-layer approach: MTD blktrans — and why

**Decision: `mtd_blktrans` (`drivers/mtd/mtd_blkdevs.c` + a `mtd_blktrans_ops`),
modelled on `mtdblock` / `ftl.c` / `inftl`.** Rationale:

- **It is exactly the abstraction for "a translation layer on top of an MTD."**
  We register one `struct mtd_blktrans_ops`; the core calls our `add_mtd()` for
  every MTD, we bind to the raw NAND we understand, build the L2P, and call
  `add_mtd_blktrans_dev()`. The core creates the gendisk, request queue
  (blk-mq under the hood), partition scanning, refcounting and char/block glue.
  We only implement a per-sector `readsect()`.

- **Read-only falls out for free.** `add_mtd_blktrans_dev()` /
  `blktrans_open` force the device read-only whenever `tr->writesect` is NULL
  (`mtd_blkdevs.c`: `if (!tr->writesect) new->readonly = 1;`). We omit
  `writesect` entirely and also set `mbd.readonly = 1`. There is no code path
  that can write the NAND — structurally enforced, which is what Phase B wants.

- **Precedent is identical in shape.** `mtdblock` is a trivial 1:1 translation;
  `ftl.c`/`inftl.c` are *real* FTLs with an L2P map behind `readsect`. Ours is
  the `ftl.c` shape (L2P lookup then read the mapped page) minus all write/GC.

- **We are not an MTD.** The vendor logical space is *not* a NAND with erase
  semantics; exposing it as another `mtd_info` (mtdblock-style) would be lying
  about erase/OOB. A blktrans device is a plain disk of 512-byte sectors, which
  is precisely what a mounted rootfs wants. `/dev/rkftl0` can be
  `mount -o ro`'d or partition-scanned directly.

**Rejected alternatives:**
- *Raw blk-mq / bio-based block device* (like `null_blk`/`brd`): more
  boilerplate (tag set, queue, gendisk lifecycle, MTD-suspend interlock) that
  `mtd_blkdevs.c` already solves and that every other MTD FTL reuses. No upside.
- *A second `mtd_info` translation (mtdblock-style over a fake MTD)*: wrong
  semantic layer, and mtdblock itself already runs on blktrans anyway.

`tr->blksize = 512` (host sector), `part_bits = 3` so the logical device can be
partition-scanned. `major = 0` → dynamic major (mtd_blkdevs calls
`register_blkdev(tr->major,...)` and stores the returned major).

---

## 2. Module architecture

```
 block layer (mount ro, partition scan)
        │  readsect(block=512B sector)
        ▼
 rk_ftl_readsect()            ← LPN = block/16, sec = block%16
        │  L2P[LPN] → PPA
        ▼
 rk_ftl_read_page()  ── mtd_read() (ECC, descrambled)  ─┐
 rk_ftl_read_oob()   ── mtd_read_oob(PLACE_OOB)         │  rockchip-nand-controller
        ▲                                               ▼
   boot map-load (add_mtd):                        raw NAND (MTD)
   scan_sysblk → load_sysinfo → map_recovery
              → expand_l2p → overlay_open
```

One `struct rk_ftl` per bound MTD, embedding `struct mtd_blktrans_dev mbd`
(recover with `container_of`, `mbd_to_ftl()`). Holds geometry, `sysinfo`,
the level-1 `region_to_ppa[]`, the flat `l2p[capacity]`, and a per-device
8192-byte page scratch buffer (`page_buf`, guarded by `mbd.lock`) that caches
the last decoded LPN so the 16 sectors of one page cost a single `mtd_read`.

Registration is via `module_mtd_blktrans(rk_ftl_tr)` (expands to
`module_init/exit` calling `register_mtd_blktrans`/`deregister_mtd_blktrans`).

### OOB de-rotation + version selection

- **De-rotation** (`rk_ftl_sysword_pos`, `rk-ftl.h`): the mainline driver places
  BCH step *i*'s 4 sys bytes at OOB word `(i==0)?STEPS-1:i-1`. So vendor step 0
  (the magic) is at OOB **word 7 / byte 28**, steps 1..3 at words 0,1,2. We read
  `mtd_read_oob` with `MTD_OPS_PLACE_OOB` (mode 0) — the same processed OOB the
  `MEMREADOOB` ioctl gives `tools/l2p-scan.py` — then gather words at positions
  7,0,1,2 → `{magic, version, lpn, prev_ppa}`. This mirrors `sw()` in the tool.
- **Version selection** (`rk_ftl_ver_newer`): 32-bit wrapping counter, skips
  `0xFFFFFFFF`; "a newer than b" iff `((a-b)&0xFFFFFFFF) < 0x80000001`. Used both
  in the map-block walk (later version wins per region) and the open-block
  overlay (higher version wins per LPN). Identical to `newer()` in the tool.

---

## 3. Boot map-load sequence → kernel APIs

Vendor order `FtlSysBlkInit → FtlScanSysBlk → FtlLoadSysInfo →
FtlMapTblRecovery → expand → FtlPowerLostRecovery`. Implemented in
`rk_ftl_build_map()`, invoked from `add_mtd()` **before** the block device is
added:

| Vendor fn / step | kernel fn | API used | status |
|---|---|---|---|
| `FtlScanSysBlk` (b097aef4) — scan trailing `max(param,24)` block-rows, bucket page-0 OOB by magic (F0A4/F086/F0C2) | `rk_ftl_scan_sysblk()` | `mtd_block_isbad`, `rk_ftl_read_oob` at `blk*ppb` | **TODO** (loop stubbed; sysblk_num hard-set to 24) |
| `FtlLoadSysInfo` (b097c3ec) — newest F0A4 block, last valid page → capacity, open-sblk ids, max_version | `rk_ftl_load_sysinfo()` | `rk_ftl_read_page` + header parse | **TODO** (placeholder capacity = whole device) |
| `FtlMapTblRecovery` (b097bca8) — build level-1 `region_to_ppa` from F0C2 pages, ascending version, region at OOB+0x08 | `rk_ftl_map_recovery()` | alloc + OOB walk | **allocation done; walk TODO** |
| expand → `L2P[capacity]` — read each region's map page, copy u32 LE PPA array | `rk_ftl_expand_l2p()` | `rk_ftl_read_page`, `le32_to_cpu` | **implemented** (drives off region_to_ppa) |
| `ftl_open_sblk_recovery` — overlay open superblock pages by version | `rk_ftl_overlay_open()` | per-page `rk_ftl_read_oob` + `rk_ftl_ver_newer` | **TODO** (needs open-sblk ids + per-LPN version) |

### Python → kernel function map (`tools/l2p-scan.py`)

| Python | kernel equivalent |
|---|---|
| `MEMREADOOB` ioctl loop reading OOB per page | `rk_ftl_read_oob()` via `mtd_read_oob(MTD_OPS_PLACE_OOB)` |
| `sw(o, s)` de-rotation | `rk_ftl_sysword_pos()` + `le32_to_cpup` in `rk_ftl_read_oob()` |
| `newer(a, b)` | `rk_ftl_ver_newer()` (`rk-ftl.h`) |
| `magic == DATA_MAGIC` gate, keep newest per LPN | route-A collision rule; reused in `rk_ftl_overlay_open()` |
| `lpn2phys[lpn] = (pg, ver)` dict | flat `ftl->l2p[lpn] = ppa` (route B) |
| `l2p-extract.py`: `os.pread(fd, PAGE, phys*PAGE)` | `rk_ftl_read_page()` via `mtd_read()` |
| `l2p-extract.py` hole → zero-fill | `rk_ftl_readsect()` zero-fills `PPA_INVALID` LPNs |
| map value = linear phys page | route B stores encoded PPA → `rk_ftl_ppa_to_linear()` decodes blk/page |

Note the tool takes **route A** (full per-page OOB scan) which alone rebuilds
the whole map; the kernel driver takes **route B** (authoritative map blocks +
open-block overlay) because it is O(regions) reads at boot instead of scanning
all ~950k pages. Route A remains the fallback/validation oracle.

---

## 4. Memory budget for `L2P[capacity]`

- Device ≈ 7.4 GB → capacity ≈ 950k LPNs → `950e3 × 4 B ≈ 3.8 MB` for the flat
  L2P. Allocated with **`vmalloc`** (physically fragmented is fine, never DMA'd,
  and 3.8 MB contiguous kmalloc would be fragile). Initialised to
  `0xFFFFFFFF` (all-holes) so unmapped LPNs read as zeros.
- Level-1 `region_to_ppa[]`: `ceil(capacity/2048) ≈ 464` regions × 4 B ≈ **2 KB**
  → plain `kmalloc_array`.
- Scratch: one 8192-B `page_buf` + one `oobsize` (~744 B) `oob_buf` per device.
- Peak transient during expand: we reuse `page_buf` to read each map page, so no
  extra large allocation. Total steady-state ≈ **3.8 MB + ~11 KB per device.**

3.8 MB resident is acceptable on this box (the vendor FTL keeps an equivalent
table). If it ever needs trimming, the two-level `region_to_ppa` + on-demand map
page reads could serve reads without fully expanding L2P — noted as a future
option, not needed now.

---

## 5. What compiles vs. what is TODO

**Compiles / structurally complete** (against the MTD blktrans + mtd_read APIs
verified in `env/kdir`):
- Module init/exit and `mtd_blktrans_ops` registration (`module_mtd_blktrans`).
- `rk_ftl_add_mtd` / `rk_ftl_remove_dev` lifecycle, geometry derivation
  (`mtd_div_by_eb`/`mtd_div_by_ws`, `writesize`/`erasesize`), buffer alloc/free.
- `rk_ftl_readsect` — LPN/sector split, L2P lookup, page-cache, hole zero-fill,
  512-B slice copy.
- `rk_ftl_read_oob` (de-rotation, `MTD_OPS_PLACE_OOB`, EUCLEAN/EBADMSG-tolerant)
  and `rk_ftl_read_page` (`mtd_read`, EUCLEAN-tolerant).
- `rk_ftl_expand_l2p` — full region→L2P expansion from `region_to_ppa`.
- `rk_ftl_ppa_to_linear`, all PPA/version/de-rotation macros in the header.

**TODO (clearly marked in-source, each with its algorithm ref):**
- `rk_ftl_scan_sysblk` — the trailing-block scan + magic bucketing, and reading
  the real `sysblk_num = max(param,24)`.
- `rk_ftl_load_sysinfo` — the 0x30 sys-info header field offsets (b097c3ec) to
  get the **real capacity** (currently a device-size placeholder — the logical
  rootfs is smaller, so this MUST be replaced before the size is trustworthy).
- `rk_ftl_map_recovery` — the map-block walk populating `region_to_ppa`.
- `rk_ftl_overlay_open` — open-superblock overlay (needs open-sblk ids from
  sysinfo and a per-LPN version array, or re-reading the incumbent page's OOB).
- `rk_ftl_ppa_to_linear` plane/superblock nuance (see risks).

Because no full kernel build is available in this environment, the C was written
to match the exact signatures in
`env/kdir/include/linux/mtd/{mtd.h,blktrans.h}` and the `mtdblock.c` reference.
**To verify:** run `make` in `drivers/nand-rk322x/ftl/` inside the toolchain
container (same recipe as the wifi/nand modules) and confirm modpost only warns
(KBUILD_MODPOST_WARN=1) about the kernel-provided symbols
`register_mtd_blktrans`, `add_mtd_blktrans_dev`, `mtd_read`, `mtd_read_oob`.

---

## 6. Open questions / risks

**Top 2 risks:**

1. **PPA → linear-page mapping across multi-plane superblocks.**
   `rk_ftl_ppa_to_linear` currently assumes `linear = blk*pages_per_block+page`.
   The vendor groups blocks into multi-plane **superblocks**; if the box uses
   N-plane striping, `blk` indexes into that scheme and the naive formula reads
   the wrong physical page near block boundaries. Must be cross-checked against
   the route-A PPAs from `tools/l2p-scan.py` (which store the *linear* page
   directly) before reads are trusted. This is the single most likely source of
   silent data corruption. (Ref: `nand-info-struct.md` geometry, plane/die
   fields `+0x64c/+0x650`.)

2. **Sys-info parsing (real capacity + open-superblock ids).** Until
   `rk_ftl_load_sysinfo` parses the 0x30 header, capacity is a placeholder and
   the open-block overlay can't run — so any page written after the last map
   flush is served from the *stale* bulk map. Correct steady-state data needs
   both the real capacity and the overlay. Getting the header offsets wrong
   yields a wrong logical size and out-of-range or truncated reads.

**Other open questions:**
- `sysblk_num` real value (`max(param,24)`) — need the param block.
- Per-LPN version storage for the overlay: keep a parallel `u32 ver[capacity]`
  (another 3.8 MB) or re-read the incumbent page's OOB on each overlay hit
  (slower, no extra RAM). Leaning re-read since overlay touches few pages.
- Whether `MTD_OPS_PLACE_OOB` vs `MTD_OPS_RAW` is the exact match for what
  `MEMREADOOB` produced in the tool — PLACE_OOB chosen because the tool used the
  normal (descrambling, ECC) ioctl path, not RAW. Confirm on hardware.
- Bad-block handling during the linear scans (`mtd_block_isbad`) — the vendor
  scan skips bad blocks; our scan stubs must do the same.
