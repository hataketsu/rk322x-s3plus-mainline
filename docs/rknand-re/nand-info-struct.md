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

_(FTL geometry struct + exact OOB byte layout: see companion sections once agents 2/3
land.)_
