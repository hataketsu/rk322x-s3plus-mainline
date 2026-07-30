# Phase C — mainline FTL WRITE-path design (rknand on RK322x)

**Status: DESIGN ONLY.** This document specifies what it takes to *safely program* the vendor
NAND from a mainline kernel so the box can eventually boot a 6.x kernel/rootfs written to NAND.
No code here. It mirrors the vendor `FtlWrite` machinery recovered in
`extracted/kernel-4.4/decompiled/` and honors the verified read-side facts in
[`ftl-l2p-algorithm.md`](ftl-l2p-algorithm.md), [`ftl-oob-format.md`](ftl-oob-format.md),
[`nand-info-struct.md`](nand-info-struct.md), [`phase-a-results.md`](phase-a-results.md).

> Prerequisite reality check (from [`../../drivers/nand-rk322x/FINDINGS.md`](../../drivers/nand-rk322x/FINDINGS.md)):
> the read path (descramble + 40-bit BCH + L2P) is proven. Writing back into the *vendor
> on-flash format* is the unsolved half. There is **no NAND backup obtainable from mainline**
> (raw read = EIO, hw-ECC read = garbage), so the only restorable backup is Multitool
> "Backup flash" from the vendor stack. **Do not write a single vendor block without that
> backup in hand** — see §7.

---

## 1. Scope & goal — what "boot 6.x from NAND" requires end to end

Three things must simultaneously be true:

1. **(a) A bootloader on NAND the BootROM will run**, that can load a mainline kernel. Today this
   is vendor U-Boot 2017.09 (reads rknand FTL, but cannot parse the Armbian 6.x boot layout —
   `read outside partition` + `Wrong image format for source`, FINDINGS §"NAND boot chain").
2. **(b) A kernel + rootfs on NAND** readable *both* by whatever u-boot loads the kernel *and*
   by the running kernel once it takes over its own root device.
3. **(c) An FTL at runtime** — either an in-kernel mainline FTL (a full Phase B write-capable
   port) or an initramfs that brings up a userspace/emulated FTL before pivot-root.

There are two honest paths. They differ in whether we keep interoperability with the closed
vendor format.

### Minimal-viable path (interoperable with vendor u-boot — the subject of this doc)
Mirror the vendor FTL write format closely enough that **vendor U-Boot 2017.09 keeps booting**
and reads what we wrote:
- Keep the vendor idblock + vendor u-boot + rknand FTL layout **untouched**.
- Use a mainline write path that produces byte-identical rknand on-flash structures (scramble,
  40-bit BCH, OOB metadata, map/sys blocks) so vendor u-boot's FTL (v5.0.53) still mounts the
  data partition and finds a boot script.
- Fix the boot-script/partition mismatch by writing a **legacy-compatible boot layout** (the
  boot.scr/extlinux conventions the 2017.09 u-boot understands) into the vendor rootfs region.
- This is the smallest change that yields a booting box, but it is the *hardest* engineering:
  it requires a bit-exact FtlWrite reimplementation (this document).

### Full path (mainline-native, wipes vendor format)
Erase the FTL region and switch to **mainline MTD + UBI/UBIFS** for kernel+rootfs, plus a
u-boot that reads UBI. This sidesteps the vendor FTL write entirely (no scramble/ECC matching
on fresh erase — the mainline driver is self-consistent). The real gap becomes **U-Boot NFC v6
support** (raw NAND + UBI read), a bounded MTD-in-u-boot port, not an FTL RE. See
[`step3-ftl-roadmap.md`](step3-ftl-roadmap.md) "fresh mainline path". **This path is lower
bricking risk and lower effort for a *booting* box** — but it does not preserve the ability to
interoperate with the vendor u-boot's rknand FTL, so it also needs the u-boot side solved.

**Recommendation up front:** the interoperable FtlWrite reimplementation (§§2–5) is the correct
target *only if* keeping vendor u-boot/idblock is a hard requirement. Otherwise the full MTD+UBI
path is safer. The rest of this document specifies the interoperable write path because that is
where the design risk lives.

---

## 2. Write algorithm — mirroring the vendor `FtlWrite`

Entry point `ftl_write(op, buf, lpn, sector_count)` (@`b0982xxx`, decompiled in
`ftl_write.c`). The high-level flow, cross-referenced to the recovered functions:

### 2.1 Superblock model
The FTL never writes single physical blocks; it groups one block per plane/die into a
**superblock** and writes pages across it. Context struct `CTX@0xb138215c`:
`+0x64c/+0x64d` = plane/die count, `+0x650` = per-die shift array (builds the PPA plane field),
`+0x137c` = sectors/block, `+0x137e` = sectors/page (16), `+0x1488` = per-block SLC/MLC bitmap.

There are (at least) **two open data superblocks at once**, confirmed by
`FtlPowerLostRecovery` recovering two (`iRamb0981b08 -0x288` = normal, `-0x258` = SLC/GC):
- one **normal/bulk** data superblock (MLC), and
- one **SLC / GC** superblock (pseudo-SLC).

Plus dedicated **map blocks** (`0xF0C2`), **sys-info blocks** (`0xF0A4`), and **ext-info**
(`0xF086`) in the trailing `sysblk_num = max(param,24)` block-rows.

### 2.2 Free-page / free-block picking
- **Within an open superblock:** `ftl_get_new_free_page()` (`ftl_get_new_free_page.c`) walks the
  per-plane member list (`param_1[8+idx]`, skipping `0xffff` dead planes), advancing a
  round-robin plane cursor at `param_1+5` and incrementing the page row `param_1[1]` when the
  cursor wraps `CTX+0xf90` (plane count). Returns `PPA = (block<<10) | page`, i.e. it fills a
  full page-row across all planes before advancing the row — **strictly ascending program order
  within a block** (critical for MLC, §4).
- **Temp/GC pages:** `Ftl_get_new_temp_ppa()` — same walk but honors the SLC bitmap: when
  `param_1[4]==1` (SLC superblock) and mixed-mode flag `CTX+0xf40` set, it consults the SLC page
  table at `+0x24c` and can early-terminate a block (`puVar5[2]=0`) so an SLC superblock only
  uses its SLC-legal (lower) pages.
- **New superblock allocation:** `ftl_alloc_new_data_sblk()` (`ftl_alloc_new_data_sblk.c`) is
  called when the current open superblock is exhausted. It:
  1. `ftl_update_l2p_map()` — flush the just-closed superblock's page→LPN mappings into the L2P
     region tables (see 2.4),
  2. picks a fresh free superblock (`func_0xb096a08c`), frees the old one if reused,
  3. `ftl_open_sblk_init(sblk, type)` with `type=2` for the primary data sblk (the one at
     `CTX+0x244+0x10`), `type=3` otherwise,
  4. `ftl_ext_info_flush()` then `ftl_info_flush(0)` — **persist FTL state at every superblock
     rollover** (this is a natural power-loss checkpoint boundary, §5).
- Free-block source: `FtlFreeSysBlkQueueOut()` (used by map/sys allocs in
  `Ftl_write_map_blk_to_last_page.c`); the free bitmap and VPC (valid-page-count) table live in
  the sys-info block.

### 2.3 SLC vs MLC placement
- Metadata (sys-info, map, ext-info) and the GC/critical superblock use **pseudo-SLC**
  (`bit31=SLC` in the PPA, SLC bitmap `CTX+0x1488`). SLC mode = program only the lower page of
  each MLC pair → immune to paired-page corruption (§4) and far more power-loss-robust.
- Bulk rootfs data goes to **MLC** superblocks for capacity.
- The mode is decided at superblock-init time and recorded in the per-block bitmap; the PPA
  carries `bit31` so the read path already distinguishes them (verified: `bit31=SLC` in
  [`ftl-oob-format.md`](ftl-oob-format.md)).

### 2.4 Per-page OOB metadata construction (the L2P key — must be bit-exact)
For every data page programmed, `ftl_write` fills the 16-byte OOB struct
(producer `@b0983040–b0983074`, magic literal `@b0983228`):

| Off | Field | Value on write |
|---|---|---|
| +0x00 | magic | `0xF095` (data) |
| +0x02 | superblock tag | current open-sblk id |
| +0x04 | **version** | monotonic counter, **increment per page/op**, skip `0xFFFFFFFF` |
| +0x08 | **LPN** | the logical page being written |
| +0x0C | **prevPPA** | the PPA this LPN previously mapped to (for GC/recovery) |

**Rotation caveat (fatal if missed):** mainline's `rockchip-nand-controller` exposes the per-step
sys bytes **rotated** — step0 lands at OOB offset 28, steps 1–3 at offsets 0/4/8
([`ftl-oob-format.md`](ftl-oob-format.md) §CRITICAL). The write path must place the struct words
into the *same* rotated slots so the vendor FTL (and our Phase B reader) find `magic` at step0.
Getting the rotation wrong means every page we write is invisible/garbage to the vendor u-boot.

### 2.5 Map-block updates (`FtlMapWritePage` / `Ftl_write_map_blk_to_last_page`)
The authoritative L2P lives in two-level map blocks (`0xF0C2`), one map page per **region**
(`region = LPN / (spp*128) = LPN/2048`, matching read-side `entries_per_region`):
- `FtlMapWritePage()` (`FtlMapWritePage.c`) programs a region's packed `u32 LE` PPA array as one
  page; OOB `+0x08 = region` (not LPN), `+0x0C = data checksum` (`func_0xb0971e44`, only when
  `pcRamb097a150` checksum-enable set). It advances `param_1[1]` (page cursor).
- **When to flush a region map page:** the dirty L2P regions are flushed lazily — at superblock
  rollover (`ftl_update_l2p_map` → `ftl_info_flush`), on cache writeback (`FtlCacheWriteBack`),
  and on explicit `FtlSysFlush`. Newest map page for a region wins by version (read-side route B).
- **The `0xFAF5` log-snapshot shortcut:** when a map block is nearly full
  (`page >= max_pages-1` or `*param_1==0xffff`), `FtlMapWritePage` calls
  `Ftl_write_map_blk_to_last_page()`. That writes the block's **last page** as a compact
  `{region, PPA}` log array (8 bytes/entry) with OOB `+8 == 0xFAF5`, so boot can replay the
  block's net region→PPA deltas without reading every map page, then `ftl_map_blk_gc()` recycles
  the block. Our writer must reproduce this sentinel exactly or route-B boot mis-reads the block.

### 2.6 Sys-info block updates (`ftl_info_flush` → `0xF0A4`)
`ftl_info_flush()` (`ftl_info_flush.c`) programs a sys-info page:
- increments the **sys-info version** (`*(iVar15+4)+1`, stored and echoed to `CTX-0xc74`),
- lays down the header (0x30) + **per-block VPC table** (`FtlVpcTblFlush`) + **free bitmap** +
  **max-version** watermark + open-superblock ids (so recovery knows which blocks were open),
- also flushes the **erase-count / wear-leveling table** via `FtlEctTblFlush` (ECT),
- writes via `ftl_prog_page(0, page, ...)`; when the current sys block is full it advances to the
  next sys block in the trailing region and re-seeds page 0.
Only the **newest two** `0xF0A4` blocks are trusted at boot (`FtlScanSysBlk` keeps newest 2 by
OOB+4) — so a torn sys-info write always leaves a good prior copy.

### 2.7 Wear-leveling / GC triggers
- After a burst, `ftl_write` calls `ftl_do_gc(0, span)`; when the **free-superblock count** is low
  (`uVar2 < 6`, or `< 0x20` while a flag is clear) it drives `ftl_do_gc(1,1)`/`ftl_do_gc(0,1)`
  loops and `FtlGcRefreshBlock` for read-disturb/retention refresh.
- GC copies valid pages to the SLC/GC superblock (`Ftl_get_new_temp_ppa`,
  `Ftl_gc_temp_data_write_back`), rewriting OOB with a *newer* version and `prevPPA` = source,
  so route-B "newest version wins" still resolves correctly.
- Wear-leveling is driven by the ECT table (erase counts) balanced across the free queue.

**For a mainline writer, GC/WL can be minimized** (write append-only to fresh superblocks, flush
map+sys at rollover) as long as the on-flash structures are format-legal — we do not need the
vendor's WL *policy*, only its *format*. But we must still honor VPC/free-bitmap accounting so the
vendor FTL doesn't think our blocks are free and overwrite them.

---

## 3. Randomizer + ECC on the program path

### 3.1 Order of operations
Per-page program (`ftl_prog_page` → `func_0xb0967138`, the low-level NFC program; PPA decoded by
`ftl_prog_ppa_page` into die/block/page):
1. **Set the scrambler seed for this page row.** `nandc_set_seed(page)`:
   `seed = seed_table[page & 0x7f]; if (randomizer_enabled) seed |= 0xc0000000;`
   written to `RANDMZ_CFG` = **MMIO+0x150** (v6/v8, our box). The 128×u16 table is in
   [`randomizer-seed-table.txt`](randomizer-seed-table.txt).
2. **Select BCH strength.** `nandc_bch_sel(40)` → v6/v8 `BCHCTL @ MMIO+0x0c = 0x41000 | 1`
   (40-bit). **Data area = 40-bit BCH over 1024-B steps**, 8 steps/page, 70 parity B/step + 4 sys
   B/step (592 B ≤ 744 OOB) — mainline's *native* geometry ([`nand-info-struct.md`](nand-info-struct.md) §Resolution).
3. **DMA the data + OOB and trigger.** The NFC **scrambles the data with the LFSR seed, then the
   BCH engine computes parity over the scrambled bytes**, and writes data+sys+parity to the page.
   (Read reverses: BCH-correct the scrambled bytes, then descramble — which is exactly why the
   read path needed the seed to match, verified in [`../README...`](README.md) §5b/5c.)

So: **seed first, then BCH, then program.** The randomizer must be enabled on WRITE identically to
READ — the same `page & 0x7f` index and the same `0xc0000000` enable. The BCH parity is generated
by hardware; we do not compute it in software, but we *do* control which 1024/40 geometry is
active.

### 3.2 Risk of getting scramble/ECC wrong
- **Wrong seed index or disabled randomizer on write** → the page is stored unscrambled; the
  vendor read path (and our Phase B reader) will descramble it into noise. **Unreadable NAND.**
- **Wrong BCH strength on write** (e.g. mainline's default 16-bit) → parity computed for the wrong
  geometry; every subsequent read reports uncorrectable ECC and the block gets marked bad.
- **Rotation/sys-byte mismatch** (§2.4) → OOB metadata unparseable → page invisible to FTL.
- These are *silent at write time* (program succeeds) and only surface as corruption on read —
  which is why §7 mandates immediate read-back verification through the Phase B path after every
  write to a scratch block, before trusting anything.

---

## 4. MLC paired-page hazard

MLC cells store 2 bits: each physical cell contributes to a **lower page** and an **upper page**,
and these are *not* adjacent page numbers — they are paired by the chip's internal mapping. The
hazards:
- **Programming an upper page can corrupt its already-written lower page** if power is lost mid-
  program (the paired lower page's charge is disturbed by the interrupted upper-page program).
- **Partial-page or out-of-order programming** within a block violates the "program pages in
  ascending order, once each" rule and corrupts pairs.

**Why the vendor uses pseudo-SLC for metadata:** sys-info/map/ext/boot and the GC superblock are
programmed in **SLC mode** (lower page only, `bit31=SLC`), so there is no upper page to disturb —
a power loss during a metadata write can damage at most the page being written, never a
previously-committed page. That is what makes the version/prevPPA journal (§5) *atomic-ish*.

**Safe mainline write policy (required):**
1. **SLC-only for everything we care about surviving power loss** — metadata *and*, ideally, the
   kernel/boot payload. SLC halves capacity but is the only regime where our append-only writer is
   power-safe without modeling the chip's exact pair map.
2. If MLC is used for bulk rootfs: **full-page, strictly sequential program within a block**
   (exactly what `ftl_get_new_free_page` already enforces — fill a whole page-row across planes,
   then advance), **no partial-page programming, never re-program a page**, and **treat the last
   1–2 upper pages of an open MLC block as "at risk"** on power loss (the vendor's recovery scan
   handles this via the close/pad record, §5).
3. **Never rewrite in place** — the FTL is copy-on-write by construction (new PPA + higher
   version); mainline must preserve that (no in-place OOB edits, no re-program of a committed
   page).

---

## 5. Power-loss safety

The vendor makes writes atomic-ish through **journaled, versioned, copy-on-write superblocks**:

- Every data page carries `version` (monotonic) + `prevPPA`. A new write for an LPN goes to a
  fresh page with `version = newer`, so the *old* mapping remains fully valid until the new page
  is committed. If power is lost mid-write, the new page is either fully programmed (newest wins)
  or absent (old page still authoritative). There is no in-place mutation to tear.
- **Open-superblock recovery** at boot (`ftl_open_sblk_recovery` / `FtlRecoverySuperblock`,
  driven by `FtlPowerLostRecovery` on **both** open superblocks — normal at `-0x288`, SLC/GC at
  `-0x258`): scan the open superblock page-by-page in program order; for each valid data page,
  if its version beats the current L2P mapping for its LPN, overlay it. This reconstructs
  everything written since the last map/sys flush.
- **Superblock close/pad record** (`FtlSuperblockPowerLostFix.c`): when closing (or recovering)
  an open superblock the FTL writes a sentinel page — `magic=0xF095`, `version`, but
  `LPN=0xFFFFFFFD` / `prevPPA=0xFFFFFFFE` (non-data markers) — to fence the end of valid data and
  neutralize any half-written MLC upper pages. `FtlGcPageRecovery` + `FtlSlcSuperblockCheck` clean
  up the GC superblock similarly.
- **Checkpoints:** `ftl_info_flush` (sys-info, newest-2 retained) and map flushes happen at every
  superblock rollover and on `FtlSysFlush`, so recovery only ever has to replay from the last
  flush forward across the open superblocks — bounded work.

**What the mainline write path MUST preserve** so Phase B read + `FtlPowerLostRecovery` still
reconstruct after an interrupted write:
1. **Monotonic, never-reused `version`** (wrap-skip `0xFFFFFFFF`; comparator rule
   `(vnew-vold)&0xFFFFFFFF < 0x80000001`). If we reuse or non-monotonically assign versions, "newest
   wins" resolves to the wrong page.
2. **Correct `prevPPA`** on every rewrite (old mapping for the LPN) — GC/recovery relies on it.
3. **Ascending program order within each open superblock** and the **close/pad sentinel** on
   clean close, so the recovery scan's "stop at first invalid page" logic terminates correctly.
4. **Update the max-version watermark in sys-info** so recovery's version counter resumes above
   everything on flash (else new writes collide with old versions).
5. **Keep VPC + free-bitmap consistent** — a block we wrote must not appear free, or the vendor
   FTL will erase it. Flush sys-info *after* the data it accounts for is committed (order:
   data pages → map pages → sys-info), never before.

Because all of this is append-only + versioned, the correctness bar is "our pages are
format-legal and versioned above the vendor's high-water mark" — we do **not** need to reproduce
the vendor's GC timing, only its on-flash invariants.

---

## 6. Bootability

### 6.1 How the SoC finds and loads the bootloader
- The **RK322x BootROM** does not use the FTL. It scans the **first few physical blocks** for a
  Rockchip **idblock** (IDB) structure (magic `0x0FF0AA55`-family / RC4-obfuscated header), and
  loads it with **its own hardware ECC = 60-bit** (`g_idb_ecc_bits = 60` for v6/v8, 70 for v9 —
  [`nand-info-struct.md`](nand-info-struct.md); confirmed live: u-boot banner `ECC:60`). This is a
  **separate geometry from the 40-bit data ECC** — different strength, different randomizer usage,
  boot-ROM-managed. Mainline's `boot_rom_mode` path is the one that matches idblock reads
  ([`nand-info-struct.md`](nand-info-struct.md) tail).
- The idblock loads the **miniloader / SPL**, which then loads **u-boot proper** (the vendor
  2017.09, which contains the rknand FTL and reads the rest of NAND).

### 6.2 What u-boot needs / what we would write
Two sub-options, matching §1:
- **Keep vendor idblock + miniloader + u-boot (interoperable path):** we write **nothing** in the
  boot blocks. The BootROM→miniloader→vendor-u-boot chain is left byte-intact (and is
  family-identical to Multitool's `bsp/legacy-uboot.img`, FTL 5.0.53 — FINDINGS). We only make the
  **rootfs region** contain a boot layout the 2017.09 u-boot understands (legacy `boot.scr` +
  partition conventions) plus a 6.x kernel it can load. **Do not touch the 60-bit idblock.**
- **Mainline u-boot (full path):** write a **Rockchip idbloader** (`idbloader.img` =
  TPL/SPL wrapped by `mkimage -n rk322x -T rksd/rknand`) that the BootROM accepts at ECC:60, then
  a mainline U-Boot that has **NFC v6 raw-NAND + UBI** support. Mainline u-boot's rk322x NFC
  driver is weak/absent today (FINDINGS) — this is the porting cost. Note we would be writing the
  **60-bit idblock geometry**, *not* the 40-bit data geometry, for those first blocks — a second,
  separate ECC/scramble code path from §3.

**Key takeaway:** the idblock (60-bit) and the FTL data area (40-bit) are two different on-flash
formats. Any boot-block write must use the 60-bit `boot_rom_mode` geometry; getting this wrong
bricks the BootROM's ability to find a loader (worse than an FTL-data mistake, because it's
pre-u-boot and unrecoverable without external flashing / maskrom mode).

---

## 7. Risk register + staged rollout

### 7.1 Risk register
| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | Wrong scramble/ECC/rotation on write → pages silently unreadable | High (many params) | Data lost, but recoverable by erase | Read-back every write via Phase B before trusting; start on a free block |
| R2 | Corrupt the 60-bit **idblock/boot** blocks → box won't reach u-boot | Med (if we touch boot blocks) | **Brick** (needs maskrom/USB reflash) | Never write boot blocks in the interoperable path; if we must, verify via BootROM on a spare unit first |
| R3 | MLC paired-page corruption on power loss | Med | Adjacent committed data lost | SLC-only for anything critical; full sequential MLC program + close/pad sentinel |
| R4 | Version/VPC/free-bitmap inconsistency → vendor FTL erases our blocks or mis-resolves L2P | Med | Silent data loss on next vendor boot | Monotonic versions above watermark; write order data→map→sys; keep VPC/free bitmap exact |
| R5 | No mainline NAND backup exists → any mistake is irreversible | **Certain** | Total | **Multitool "Backup flash" full dump before ANY write** — the only restorable backup |
| R6 | `0xFAF5` map log / sys newest-2 semantics reproduced wrong → route-B boot mis-reads map | Med | Wrong rootfs mapping | Reproduce sentinel + newest-2 exactly; validate with route-B parse offline |

### 7.2 Staged, testable rollout (reversibility first)
0. **Take a full Multitool "Backup flash" dump and verify it restores** on a scratch/spare box.
   No dump, no writes. (R5)
1. **Offline first:** implement the writer to emit an rknand image *to a file*, then parse it with
   the existing route-A/route-B tools ([`../../tools/l2p-scan.py`](../../tools/l2p-scan.py) style)
   — prove round-trip in software before touching hardware.
2. **Scratch free block on hardware:** pick a block the vendor free-bitmap marks free, program a
   handful of SLC data pages (known LPNs, synthetic content) with full OOB + scramble + 40-bit
   BCH. **Read them back through the Phase B read path**; require zero ECC errors and exact
   content/OOB match. (R1, R3)
3. **Map/sys structures:** write a map region page (`0xF0C2`) and a sys-info page (`0xF0A4`) to
   scratch blocks; parse them with route-B logic offline; confirm region/version/VPC/free-bitmap
   fields resolve. Do **not** yet let the vendor FTL see them. (R4, R6)
4. **Power-loss injection (bench):** interrupt a scratch-block write; reboot vendor stack; confirm
   `FtlPowerLostRecovery` reconstructs to the pre-write state (old mapping intact). (R3, R4)
5. **Non-boot data partition only:** write real content into a *non-boot, non-critical* logical
   region and boot the vendor stack read-only to confirm it mounts and sees it.
6. **Boot layout, last:** only after 1–5 pass, write the legacy-compatible boot script/kernel into
   the rootfs region — still **never** touching the 60-bit idblock/miniloader. Keep the ability to
   restore the Multitool dump at every step. (R2)

**Reversibility is the governing constraint:** every step above is on free/scratch blocks or
non-boot data until the very last, and a verified Multitool backup is the safety net. Bricking
risk is real and, for the idblock, external-only to recover — so the idblock stays untouched
unless we deliberately move to the full mainline-u-boot path with a spare unit to prototype on.
