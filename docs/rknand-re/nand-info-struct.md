# rknand — NFC controller context struct & transfer geometry

Consolidated struct/register/geometry map recovered from the vendor 4.4 kernel
(Ghidra decompile + objdump). Base pointer `S` is the single global controller/FTL
context; MMIO register block base is `*(S+0x210)`.

## Controller context struct (field map)

| Offset | Type | Meaning |
|---|---|---|
| `+0x204` | u8 | NFC version tag: **6** (default), 8, or 9 — selects v9 vs v6/v8 datapath |
| `+0x210` | ptr | MMIO register base pointer (NFC iomem) |
| `+0x244` | — | FTL superblock info |
| `+0x248` | u8 | ECC strength in bits (0x10=16,0x18=24,0x28=40,0x3c=60,0x46=70); threshold `<0x19` picks per-step spare stride |
| `+0x67b` | u8 | v9 flag (1 iff version==9) |
| `+0x67c` | ptr | 0x800-byte OOB/spare bounce buffer (v6/v8; SW scatter/gather per step) |
| `+0x680` | ptr | transfer data buffer (virtual) |
| `+0x684` | ptr | transfer OOB buffer (virtual) |
| `+0x688` | ptr | data DMA-mapped address |
| `+0x68c` | ptr | OOB DMA-mapped address |
| `+0x690` | u32 | DMA-in-progress / mapped flag |
| `+0x698` | u8 | randomizer (scrambler) enable |
| `+0x69a` | u16 | per-transfer OOB/metadata size (masked 0x7ff) |
| `+0x6f8` | ptr | chip_if / flash-geometry struct pointer |
| `+0x64c` | u8 | plane / ecc-list count |
| `+0x650` | arr | ecc-mode list |
| `+0xf41` | u8 | geometry byte |
| `+0xf44` | u32 | device capacity / density (sectors) |

Version detection (`nandc_init`): reads MMIO ID regs `param[0x58]` (`+0x160`) and
`param[0x20]` (`+0x80`), upgrades tag 6→8→9 by comparison. **RK3229 box = v6.**

## MMIO register map (relative to `*(S+0x210)`)

| Purpose | v9 | v6/v8 |
|---|---|---|
| FMCTL (CS / global, ready bit 0x200) | `+0x00` | `+0x00` |
| interface / DDR-mode fields | `+0x08,+0x14..0x16` | `+0x4c..0x4e,+0x56` |
| DDR timing | `+0x50` | `+0x130` |
| **randomizer seed (RANDMZ_CFG)** | `+0x208` | `+0x150` |
| **BCH control** | `+0x20` (`mode<<25 \| 1`) | `+0x0c` (`\| 0x1000`) |
| transfer trigger (start `\|4`) | `+0x10` | `+0x08` |
| DMA cfg (sector-count + OOB-size) | `+0x30` | `+0x0c` |
| transfer-issue (data-size/sector field) | `+0x10` | `+0x10` |
| data DMA-addr | `+0x34` | `+0x14` |
| OOB/ECC DMA-addr | `+0x38` | `+0x18` |
| IRQ enable / mask (xfer=b1, ready=b2) | `+0x124`/`+0x120` | `+0x170`/`+0x16c` |
| per-step BCH status array | `(step+0x54)*4` | `(step+8)*4` |
| per-CS I/O window | `MMIO + (cs+8)*0x100` | same |

## Transfer geometry (the ECC-fit resolution)

`nsteps = (param+1 & 0x7f) >> 1`; **each hardware BCH step = 1024 data bytes** →
8 steps per 8192-byte page.

- **v9**: data DMA = `nsteps·1024` B; OOB DMA = `nsteps·4` B (HW packs 4 metadata
  bytes/step; BCH parity in a HW-managed area). OOB/metadata size written to DMA-cfg
  bits [26:16] from `S+0x69a`.
- **v6/v8 (our box)**: data DMA = `nsteps·1024` B; **OOB DMA = `nsteps·128` B** — the
  NFC transfers a **128-byte spare region per step** into the `+0x67c` bounce buffer, and
  software de-interleaves per step at stride **0x40 (strength≥25) / 0x80**. So per 1024-B
  data step the controller handles a **128-byte spare** (4 sys bytes + BCH parity), not
  the ~93 bytes mainline assumes from 744/8. This is how 60-bit BCH fits: the NFC reads a
  larger hardware spare per step than the chip's nominal 744-byte OOB — i.e. it uses part
  of what mainline treats as main/OOB differently.

> This directly explains why mainline (`ecc->steps·ecc->bytes ≤ oobsize`, 744) rejects
> 60-bit: mainline counts parity against the 744-byte OOB, but the vendor NFC moves a
> 128-B/step spare (1024 B/page of spare) via DMA. Matching means replicating the v6
> 128-B/step spare transfer + BCH status decode, not mainline's OOB-packing model.

## FTL geometry struct (CTX = static .bss @ 0xb138215c)

Ghidra's many `iRamb09xxxxxx` names are literal slots holding `CTX + bias`; family-B
accesses `base + (-0xcXX)` = `CTX + 0x1ff8 - 0xcXX`. Absolute offsets from CTX:

| Offset | Field |
|---|---|
| `+0x004` | pages per block |
| `+0x64c` / `+0x64d` | plane/die count |
| `+0x650` | u8[8] per-die shift-value array (builds FlashMask) |
| `+0x658` | u32[] per-die capacity (sectors) |
| `+0x6f8` | chip-interface struct ptr (0x100-B records) |
| `+0xf40` | SLC/mixed-mode enable flag |
| `+0xf41` | **BCH strength code** (0x3c=60, 0x28=40, 0x18=24, 0x10=16) |
| `+0xf44` | capacity/density (512-B sectors) |
| `+0x1370` | ECC/mode = 5 (MLC) / 1 (SLC) |
| `+0x1374` | spare/ECC region size per page (0x1100 default / 0x280 special) |
| `+0x137c` | sectors per block |
| `+0x137e` | **sectors per page** (used by ftl_read/write) |
| `+0x1382` | **page data bytes = sectors_per_page << 9** |
| `+0x1488` | ptr to per-block SLC/MLC mode bitmap (1 bit/block) |
| `+0x14f8` | LPN exposed to block layer |

Chip-interface record (via `CTX+0x6f8`): `+7`=manufacturer ID, `+8`=cell-type (==2 alt
capacity), **`+9`=sectors per page** (page size in 512-B units, = `FlashGetPageSize`),
`+0xd/+0xe`=block geometry, `+0x17`=multi-plane/2× density flag.

### 11-byte FLASH_INFO descriptor (`ftl_read_flash_info`)

| Off | Field | Source | This part |
|---|---|---|---|
| [0] u32 | FlashSize (512-B sectors) | `CTX+0xf44` | density |
| [4] u16 | BlockSize (sectors/block) | `chip[9]×CTX[4]` | e.g. 4096 |
| [6] u8 | PageSize (**sectors/page**) | `chip[9]` | ~14 (see below) |
| [7] u8 | ECCBits | `CTX+0xf41` | **0x3c = 60** |
| [8] u8 | AccessTime | const | 0x20 |
| [9] u8 | Manufacturer | `chip[7]` | Micron |
| [10] u8 | FlashMask | OR(1<<CTX[0x650+i]) | die mask |

## Resolution of the "60-bit doesn't fit 744 OOB" paradox — it's **40-bit data ECC**

The paradox dissolves: **the data area is 40-bit BCH, not 60.** The `60` (U-Boot `ECC:60`)
is `g_idb_ecc_bits` — the **idblock / boot** ECC strength (a separate geometry the
BootROM/miniloader uses for the first blocks). The FTL **data** area uses **40-bit BCH**.

Proof: `nand_flash_init` (b096935c) matches the chip READ-ID against an 81-entry × 32-byte
parameter table at `0xb1228a20`, copies the entry to `g_nand_para_info` (b122745c), and
feeds **para byte[20] (ECC-bits)** to `nandc_bch_sel`. The entry for our chip —
ID `2C 64 44 4B A9` (Micron MT29F64G08CBABAWP) — has **byte[20] = 0x28 = 40-bit**. Every
8 KB Micron entry is 24/40-bit; none is 60. `g_nandc_ecc_bits`(b1374b08)=40 (data);
`g_idb_ecc_bits`(b1374f18)=60 v6/v8, 70 v9 (boot only).

On-flash per-step layout (8 steps per 8192-B page):

```
[ 1024 data | 4 sys | 70 parity ]  × 8      (parity written to OOB by the HW BCH engine)
OOB used = 8 × (4 + 70) = 592 B  ≤ 744 B  ✓   (fits, 152 B slack)
```

BCH step = 1024 B (`nandc_xfer_start`: data DMA `sectors<<10`, sys DMA `sectors<<2` = 4 B/
step). `ecc->bytes = ceil(40·fls(8·1024)/8) = 40·14/8 = 70`. mainline auto-selects 40 here
(largest strength ≤ ⌊(744/8−4)·8/14⌋ = 50 that the HW supports) — so its native 8×1024 /
40-bit / 4-sys layout already **matches the vendor data geometry**. The only thing mainline
was missing is the **randomizer** (§3 / patch 0002).

### VERIFIED — clean reads

`descramble (patch 0002) + skip-bbt (patch 0001) + DT nand-ecc-strength=40, step=1024`
reads vendor data pages with **zero ECC errors**, byte-perfect filenames, 16.6 MB/s
(vs 1.7 MB/s and corrupt bytes at the wrong strength). Sample:
[`clean-read-verification.txt`](clean-read-verification.txt). **Step 2 (ECC geometry) is
done — mainline reads the vendor rknand NAND data area losslessly.**

The RK **idblock/boot** blocks are the separate 60-bit (v6) / 70-bit (v9) case — handled by
mainline's existing `boot_rom_mode` path, not this data geometry.
