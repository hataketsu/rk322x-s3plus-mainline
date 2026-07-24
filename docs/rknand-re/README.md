# Reverse-engineering the Rockchip `rknand` FTL & NFC (RK322x / RK3229)

A clean-room-style analysis of Rockchip's **closed `rknand`** flash stack — the piece that
mainline Linux lacks and that keeps RK322x TV-box NAND on the ancient 4.4 kernel. Goal:
document the NAND-flash-controller (NFC) programming, the ECC scheme, the **data
randomizer**, and the **FTL** on-flash format well enough that a mainline driver could one
day read/port it.

> Scope note: this documents *interfaces and formats* (register offsets, ECC parameters,
> the randomizer seed scheme, FTL structure) recovered from the vendor 4.4 kernel. The raw
> disassembly / decompiler output and the vendor binary are **not** redistributed (kept in
> the repo's git-ignored `extracted/`). Only original notes and the analysis tooling are
> published here.

## Source & method (reproducible)

- **Binary**: `vmlinuz-4.4.194-rk322x` from the Armbian 21.05.1 legacy image (the last
  build with the vendor `CONFIG_RK_NAND=y` blob compiled in). It's an ARM `zImage`; the
  kernel is gzip-compressed at offset `0x47b8` — `dd | gunzip` yields the raw `Image`.
- **Symbols**: the image ships `System.map-4.4.194-rk322x` — **the full symbol table**
  (101 317 symbols), including every `rknand`/`ftl_`/`nandc_` function by name. This is
  what makes the RE tractable.
- **Tooling**: Ghidra 12 headless. Load the raw `Image` as `ARM:LE:32:v7` at base
  `_text = 0xb0008000`, apply `System.map` as labels, disassemble + decompile the ~212
  `ftl_*` / `rk_nandc_*` / `nandc_*` / `Ftl*` functions. Scripts: [`ghidra/`](ghidra/)
  (`RknandRE.java` + the run wrapper). 183/212 functions decompiled cleanly.

```bash
# extract the kernel
dd if=vmlinuz-4.4.194-rk322x bs=1 skip=18360 | gunzip > Image
# headless RE (see ghidra/run.sh)
analyzeHeadless <proj> rknand -import Image -loader BinaryLoader \
  -loader-baseAddr 0xb0008000 -processor ARM:LE:32:v7 -noanalysis \
  -postScript RknandRE.java System.map <targets> <outdir>
```

## 1. NFC controller versions

`nandc_init()` detects three hardware variants and stores the tag at `ctx[0x204]`:

| tag | mainline name | notes |
|----:|---------------|-------|
| 6 | `nfc_v6` (`rockchip,rk2928-nfc`) | **this box (RK3229)** — matches the mainline of_match |
| 8 | `nfc_v8` (`rv1108-nfc`) | |
| 9 | `nfc_v9` (`px30-nfc`) | different register layout (BCHCTL/randomizer moved) |

`param` passed to `nandc_init` **is** the ioremapped MMIO base (`0x30030000` on RK3229);
functions index it as a `u32[]`.

## 2. ECC (BCH) — `nandc_bch_sel(strength)`

Supported strengths: **16 / 40 / 60 / 70-bit** BCH. Encoding differs by controller version:

**v9** — `BCHCTL @ MMIO+0x20`:
```
reg[0x10] = 1
sel = (strength==60)?3 : (strength==40)?2 : (strength==70)?0 : 1
BCHCTL(+0x20) = (sel << 25) | 1        // sel in bits[26:25], bit0 = enable
```

**v6 / v8** — `BCHCTL @ MMIO+0x0c`:
```
reg[0x08] = 1
strength 16 -> 0x1000 & ~0x10          // = 0x1000
strength 24 -> 0x1010
strength 40 -> 0x41010 & ~0x10         // = 0x41000
other/60/70 -> 0x41010
BCHCTL(+0x0c) = value | 1              // bit0 = enable
```

This unit's vendor stack uses **60-bit BCH** (confirmed by U-Boot `ECC:60` and
`g_nandc_ecc_bits`). The idblock uses a **separate** strength (`g_idb_ecc_bits`), which is
why block 0 reads differently. 60-bit BCH over 1024-byte sectors needs ~105 ECC bytes/step
→ 8×105 = 840 B > the chip's 744 B OOB under *standard* mainline BCH; the vendor fits it
because its OOB/data layout and sector geometry differ from mainline's assumption. **This
is the core reason mainline `rockchip-nand-controller` can drive the controller but cannot
decode vendor pages.**

## 3. Data randomizer / scrambler — `nandc_set_seed(page)`

The single most important finding for *reading* vendor data. Every page is scrambled with a
per-page seed:

```
seed = seed_table[page & 0x7f]         // 128-entry u16 table in the driver
if (randomizer_enabled) seed |= 0xc0000000
v9  : RANDMZ_CFG @ MMIO+0x208 = seed
v6/8: RANDMZ_CFG @ MMIO+0x150 = seed
```

So the scrambler is keyed by a **128-entry seed table indexed by `page & 0x7f`**, with the
top two bits (`0xc0000000`) enabling it. A mainline reader must reproduce this exact
sequence per page or every read is noise — matching our live probe, where hw-ECC reads
returned garbage.

The actual table was recovered from the driver (`0xb0d6b9bc`, the pointer loaded in
`nandc_set_seed`) and is published as the descrambler key:
[`randomizer-seed-table.txt`](randomizer-seed-table.txt) (128×u16, starts
`576a 05e8 629d 45a3 …`).

## 4. Register map (MMIO base `0x30030000`, u32 offsets)

Recovered from `nandc_init` / `nandc_set_seed` / `nandc_bch_sel` / `nandc_get_chip_if`:

| offset | symbol | purpose |
|-------:|--------|---------|
| `+0x00` | `NANDC_FMCTL` | flash-memory control (CS, mode) — init `0x80100` (v9) / `0x1000100` (v6/8) |
| `+0x04` | `NANDC_FMWAIT` | timing (`0x1041`) |
| `+0x08` | `NANDC_FLCTL` | flash control (`0x2081`) |
| `+0x0c` | `NANDC_BCHCTL` (v6/8) | BCH strength+enable |
| `+0x20` | `NANDC_BCHCTL` (v9) | BCH strength+enable |
| `+0x150` | `NANDC_RANDMZ_CFG` (v6/8) | per-page randomizer seed |
| `+0x208` | `NANDC_RANDMZ_CFG` (v9) | per-page randomizer seed |
| `+(cs+8)*0x100` | per-CS data/BCH window | `nandc_get_chip_if(cs)` |

`NANDC_DLL_CTL_REG0/1`, `NANDC_FMWAIT_SYN` also present (DDR/DLL tuning).

## 5. FTL architecture (from the 183-function decompile)

`rknand` is a full page-mapped FTL, not a raw-MTD layout. Key subsystems (by symbol group):

- **Geometry / init**: `FtlInit`, `FtlConstantsInit`, `FtlSysBlkNumInit`, `FtlGetCap`
  (capacity at `ctx+0xf44`), `ftl_get_density`.
- **L2P mapping**: `ftl_get_ppa_from_index` (logical index → physical page via per-
  superblock 16-bit LPA tables), `ftl_update_l2p_map`, `FtlL2PDataInit`, `FtlLoadMapInfo`,
  `FtlMapWritePage`, `ftl_map_blk_gc`.
- **Superblocks**: `ftl_alloc_sblk` / `ftl_free_sblk` / `ftl_open_sblk_*` — the FTL groups
  physical blocks across planes/dies into "superblocks"; `ftl_sblk_lpa_tbl`, `ftl_sblk_vpn`.
- **Bad-block mgmt**: `FtlMakeBbt`, `FtlLoadBbt`, `FtlLoadFactoryBbt`, `FtlBbmMapBadBlock`,
  `FtlBbt2Bitmap` — factory BBT + runtime BBM.
- **Garbage collection**: 13 `FtlGc*` functions (refresh/scan/free/recovery).
- **Power-loss recovery**: `FtlPowerLostRecovery`, `FtlSuperblockPowerLostFix`,
  `ftl_fix_nand_power_lost_error` — journaled superblocks with recovery scan.
- **Vendor partition**: `FtlVendorPartRead/Write` — the "vendor storage" (MAC, serial,
  keys) seen as `rknand vendor storage init ok` in dmesg.
- **Metadata**: `ftl_info_blk_init` / `ftl_info_flush` (system-info block) and
  `ftl_ext_info_*` (extended info) hold the mapping tables + FTL state on flash.

`FtlRead`/`FtlWrite`/`FtlDiscard` are the block-device entry points (dispatched via a
function-pointer table) that the kernel's `rk_nand` block driver calls.

## 5b. VERIFIED: reading vendor NAND from mainline

The randomizer finding is **confirmed on hardware**. Mainline's driver already exposes the
exact `randmz_off` we RE'd (`0x150` for v6/v8, `0x208` for v9) but *disables* the randomizer
(`rk_nfc_hw_init` writes 0). Patch it to program the vendor seed per page
([`../../drivers/nand-rk322x/patches/0002-*.patch`](../../drivers/nand-rk322x/patches/)):

```c
if (!boot_rom_mode)
    writel(rk_nand_randmz_seed[page & 0x7f] | 0xc0000000,
           nfc->regs + nfc->cfg->randmz_off);
```

A raw read of `/dev/mtd0` then recovers **real filesystem content** — e.g. LibreOffice
resource paths and ZIP `PK` headers from the 6.x rootfs written to the NAND:

```
chart2/res/valueaxisdirect3d_52x60.svgPK
cmd/32/connectorcurvearrowend.svgPK
xmlsecurity/res/certificate_40x56.svg
```

(sample: [`descramble-verification-sample.txt`](descramble-verification-sample.txt)).
Without the patch the same region is pure noise. A few bytes are still corrupted because
the ECC geometry isn't matched yet (16-bit mainline vs 60-bit vendor) — that's the next
item — but the **randomizer + seed table are proven correct**, and mainline can now *read*
vendor-scrambled NAND. As far as we know this is the first time that's been demonstrated.

## 5c. Step 2 — matching the ECC geometry: **SOLVED (data = 40-bit BCH)**

The apparent "60-bit doesn't fit 744 B OOB" problem was a red herring: **the data area is
40-bit BCH.** The `60` is the *idblock/boot* ECC (`g_idb_ecc_bits`), a separate geometry.
The vendor data ECC (`g_nandc_ecc_bits`) comes from the chip's entry in the parameter table
at `0xb1228a20` — for Micron `2C 64 …` it is **0x28 = 40-bit**. Full details + struct/
register maps: [`nand-info-struct.md`](nand-info-struct.md).

Layout: 8 steps/page × `[1024 data | 4 sys | 70 parity]` = 592 B OOB ≤ 744 ✓. This is
mainline's **native** 8×1024 / 40-bit / 4-sys layout — the only missing piece was the
randomizer (§3, patch 0002).

**VERIFIED clean:** `patch 0001 + 0002 + DT nand-ecc-strength=40, step=1024` reads vendor
data pages with **zero ECC errors**, byte-perfect filenames, 16.6 MB/s
([`clean-read-verification.txt`](clean-read-verification.txt)). Mainline now reads the
vendor rknand NAND **losslessly** — steps 1 (descramble) and 2 (ECC) both done.

## 6. What a mainline port would need

To **read** existing rknand data from mainline (not just drive the controller — which
`rockchip-nand-controller.c` already does):

1. **Randomizer** — replicate the 128-entry seed table + `page&0x7f` indexing + the
   `0xc0000000` enable. (The table constants still need dumping from the driver `.data`.)
2. **ECC geometry** — match the vendor's sector/OOB layout for 60-bit BCH so the hw ECC
   engine validates vendor pages (mainline's default layout doesn't fit 744 B OOB).
3. **FTL parsing** — locate & parse the system-info block (`ftl_info_*`), rebuild the L2P
   map (`ftl_get_ppa_from_index` logic) and BBT to translate logical→physical.

Items 1–2 are bounded and tractable; item 3 (the FTL) is the large effort. This is why
"6.x from NAND" (see [`../../drivers/nand-rk322x/FINDINGS.md`](../../drivers/nand-rk322x/FINDINGS.md))
remains unsolved — but this document turns it from a black box into a mapped one.

## Files

- [`ghidra/RknandRE.java`](ghidra/RknandRE.java) — headless: apply symbols, disassemble,
  decompile targets.
- [`ghidra/run.sh`](ghidra/run.sh) — reproducible run wrapper.
- Symbol list & raw decompiler output: git-ignored under `extracted/kernel-4.4/`
  (proprietary-derived — regenerate locally from the legacy image).
