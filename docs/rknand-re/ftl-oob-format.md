# rknand FTL — per-page OOB metadata format (L2P key)

Each data/system page carries a **16-byte metadata struct** in the NFC "sys" bytes
(4 bytes per 1024-B BCH step, so it occupies the sys bytes of steps 0–3). This is the key
to rebuilding the logical→physical (L2P) map from an OOB scan (step 3, route A).

## The struct (vendor step order)

| Off | Width | Field | Notes |
|---|---|---|---|
| +0x00 | u16 | **magic** | data=`0xF095`, sys-info=`0xF0A4`, ext=`0xF086`, map=`0xF0C2`, erased=`0xFFFF` |
| +0x02 | u16 | block / superblock tag | per open block |
| +0x04 | u32 | **version / sequence** | monotonic; newest-wins via wrap-around compare `((vnew-vold)&0xFFFFFFFF) < 0x80000001` |
| +0x08 | u32 | **LPN** (logical page number) | the L2P key |
| +0x0C | u32 | **prev PPA** | previous physical addr (GC/recovery) |

**PPA encoding:** `blk = (ppa & 0x3FFFFFF) >> 10`, `page = ppa & 0x3FF`, bit31 = SLC-mode.

(Producer: `ftl_write` OOB fill @b0983040–b0983074, magic literal `0xF095` @b0983228.
Consumers/cross-check: `ftl_read` validates `req.lpn == sys+0x08`; `FtlRecoverySuperblock`
reads +0x00 magic, +0x04 ver, +0x08 lpn, +0x0C prevppa; `ftl_cmp_data_ver`@b0978e44 is the
version comparator.)

## CRITICAL: mainline exposes the sys bytes ROTATED

`rockchip-nand-controller.c` read path places step *i*'s 4 sys bytes at OOB position
`(i==0) ? steps-1 : i-1`. So for an 8-step page the exposed 32-byte sys region is:

```
OOB pos: [0]=step1 [1]=step2 [2]=step3 [3]=step4 [4]=step5 [5]=step6 [6]=step7 [7]=step0
```

⇒ **the 16-byte struct is NOT at OOB offset 0**; step0 (the magic) lands at OOB offset
`(steps-1)*4 = 28`. To read the struct from the mainline driver: take sys word from OOB
offset 28 (=step0), then offsets 0,4,8 (=steps 1,2,3).

## Empirical confirmation (this box, 40-bit read of a data page @64 MiB)

Exposed OOB (first 32 bytes):
```
ff ef 6f 03 | 46 f1 07 00 | 57 84 03 00 | 00 …(pos3-6)… 00 | 95 f0 73 03
 pos0=step1    pos1=step2    pos2=step3                        pos7=step0
```
De-rotated to vendor order:
```
step0 (magic):  95 f0 73 03 → magic 0xF095 (data ✓), tag 0x0373
step1 (ver):    ff ef 6f 03 → version 0x036FEFFF
step2 (lpn):    46 f1 07 00 → LPN 0x0007F146 = 520006
step3 (prev):   57 84 03 00 → prevPPA 0x00038457 → blk 225, page 87 (valid)
```
Clean parse: valid data magic, monotonic version, sane LPN, and a prevPPA that resolves to
a real block/page. **Route A (rebuild L2P from a per-page OOB scan) is confirmed feasible
on this hardware.**

## Route A algorithm (offline L2P rebuild — reads existing vendor data)

1. For each physical page: read raw OOB, extract the 16-byte struct (de-rotate per above).
2. Keep data pages (`magic==0xF095`, `LPN!=0xFFFFFFFF`); skip sys/map/ext/erased.
3. `map[LPN] = {PPA=this_page, version}`; on collision keep newer version (wrap-around rule).
4. Invert → logical order; emit a logical image; loop-mount (ext4) to verify.

This is what the firmware itself does for open superblocks; the authoritative boot-time map
also lives in dedicated **map blocks** (magic `0xF0C2`, data = packed PPA array per L2P
region — `FtlMapWritePage`/`FtlMapTblRecovery`), but a full OOB scan reconstructs the whole
map without parsing them.

> Scope: this route **reads existing vendor data**. For a *fresh* 6.x NAND boot, the
> lighter path is mainline MTD+UBI (no FTL) — see `step3-ftl-roadmap.md`.
