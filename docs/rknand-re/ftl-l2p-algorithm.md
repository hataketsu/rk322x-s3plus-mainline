# rknand FTL — complete L2P reconstruction algorithm

The logical→physical (L2P) map, fully RE'd from the vendor 4.4 kernel, enough to rebuild
the vendor rootfs offline from a raw NAND dump (or to reimplement the read path in a
mainline driver). Two layers: **route A** (per-page OOB scan — self-describing, sufficient
alone) and **route B** (authoritative map blocks — what the firmware trusts at boot).

## Geometry & units (verified)

- `sectors_per_page (spp) = 16` (chip param table `gNandParaInfo@0xb1228a00`, entry
  `2C 64 44 4B A9` byte[+9]=0x10). Host payload per physical page = `spp<<9 = 8192 B` (the
  **full** main area; ECC/meta live in the 744 B OOB, no in-band carve-out).
- **LPN = logical PAGE index** (not sector): `ftl_read`/`ftl_write` do `LPN = sector / spp`.
  → **host image byte offset of a page = `LPN * 8192`.**
- **One LPN ↔ one physical page** — `log2phys(lpn)` stores a single u32 PPA per LPN, no
  per-logical-page plane striping. Placement is fully determined by each page's own OOB.LPN.
- **PPA decode:** `blk = (ppa & 0x3FFFFFF) >> 10`, `page = ppa & 0x3FF`, bit31 = SLC.
- **Version:** OOB `+0x04`, 32-bit wrapping counter (skips `0xFFFFFFFF`); newest wins by
  modular compare (`a` newer than `b` iff `(a-b)&0xFFFFFFFF < 0x80000001`).
- **Total logical pages** = `FtlGetLpn()` (device is ~7.4 GB → ~950k LPNs).

## Route A — per-page OOB scan (self-describing, sufficient)

Every data page's OOB (16-byte struct; mainline exposes it rotated — step0 at OOB off 28)
carries `magic=0xF095, version, LPN, prevPPA`. Rebuild:

```
for each physical page P with OOB.magic==0xF095 and OOB.version!=0xFFFFFFFF:
    L = OOB.LPN
    if L not seen, or OOB.version newer than stored:  map[L] = (P, version)
image[L*8192 : L*8192+8192] = data(map[L].P)      # holes (unseen L) read as zeros
```

This is exactly what the firmware's `ftl_open_sblk_recovery` / `FtlRecoverySuperblock` do
for open blocks. Tool: [`../../tools/l2p-scan.py`](../../tools/l2p-scan.py) (validated on
real NAND: 100% data magic, sane LPNs, version monotonic).

## Route B — authoritative map blocks (what the firmware loads at boot)

Boot path: `FtlSysBlkInit → FtlScanSysBlk → FtlLoadSysInfo → FtlLoadMapInfo
(FtlL2PDataInit + FtlMapTblRecovery) → FtlPowerLostRecovery (open-block overlay)`.

1. **Locate** (`FtlScanSysBlk`, b097aef4): scan the **trailing** `sysblk_num = max(param,24)`
   block-rows (`first = total_blocks - sysblk_num`) across all planes; read each non-bad
   block's page-0 OOB; bucket by `OOB[0]` magic: `0xF0A4`=sys-info (keep newest 2 by
   OOB+4), `0xF086`=ext, `0xF0C2`=map (collect, sort by version), `0xFFFF`/other=free.

2. **Sys-info** (`FtlLoadSysInfo`, b097c3ec): newest `0xF0A4` block, read last valid page
   (magic + DATA[0..3]==signature). Header(0x30) → capacity, open-superblock ids,
   max-version; then per-block VPC table + free bitmap. Gives `capacity` = total LPNs.

3. **Bulk map** (`FtlMapTblRecovery`, b097bca8): two-level.
   - `entries_per_region = page_bytes/4 = spp*128` (u32 PPAs per map page).
   - `region = LPN / entries_per_region`, `offset = LPN % entries_per_region`.
   - A `0xF0C2` map page's DATA = packed `u32 LE` PPA array for one region; its OOB `+0x08`
     = region number (not LPN), `+0x0C` = data checksum (not prevPPA).
   - Process map blocks in ascending version; for each `0xF0C2` page (skip the `0xFAF5`
     log-snapshot page), `region_to_ppa[region] = PPA(page)` (later wins). Sealed blocks
     may carry an 8-byte `{region,PPA}` log in their last page (OOB+8==`0xFAF5`) as a
     shortcut.

4. **Expand** to `L2P[capacity]` (init `0xFFFFFFFF`): for each region with a valid PPA,
   read that map page's u32 array → `L2P[region*epr + i] = arr[i]` (skip `0xFFFFFFFF`).

5. **Overlay open blocks** (`ftl_open_sblk_recovery`): scan the open data superblock(s)
   page-by-page in program order; for each valid data page, if its `version` beats the
   current mapping for its `LPN`, `L2P[LPN] = PPA(page)`. Open writes are post-flush → they
   override the bulk map. (Two open blocks recovered: normal + SLC/GC.)

**Precedence, one line:** highest OOB `+0x04` version wins per LPN — map blocks are the
flushed baseline, open-superblock pages override.

## Practical note
Route A alone reconstructs the whole map (it is the union of what the map blocks + open
recovery produce, resolved by version). Route B is the faster/authoritative path and the
one to mirror in a kernel driver; use A when you can read every page's OOB. Map-context
struct: RAM `0xb1383f5c` (ptr `0xb097c188`); `region_to_ppa` = the level-1 table.
