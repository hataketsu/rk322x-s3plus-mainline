# Track B — NAND boot on the current kernel

## The problem in one paragraph

The RK322x boot ROM can load a first-stage loader (idbloader/miniloader) from raw NAND,
so **the SoC itself can boot from NAND**. What's missing is a **Linux driver** for the
NAND controller under kernel 6.6: without it the running system cannot mount a rootfs
that lives on NAND, and `steP-nand` installs have nowhere to put (or read) the OS.
Under 4.4 this was handled by Rockchip's closed `rknand` blob.

## Why the 4.4 blob can't just be reused

- `rknand.ko` is **binary-only**, compiled against the 4.4 kernel's internal ABI
  (block layer, `struct request`, MTD/gendisk APIs). Those APIs changed fundamentally by
  6.6. A `.ko` from 4.4 will not even load (vermagic + unresolved symbols), and there is
  **no source** to recompile.
- So "porting the driver" = **writing a new mainline driver**, not rebuilding the blob.

## The two sub-problems

1. **Boot chain (steP-nand).** Get idbloader + u-boot + a boot partition onto NAND via
   the vendor miniloader, as Multitool's *"Burn Armbian image via steP-nand"* does. This
   part is largely **tooling**, reusable from Multitool. Reference blobs already seen on
   the old Multitool card: `bsp/uboot.img`, `bsp/legacy-uboot.img`, `bsp/trustos.img`.
2. **Runtime NAND access (the hard part).** A kernel driver for
   `nand-controller@30030000` so Linux sees the NAND as MTD/block and can host rootfs.

## RE plan for sub-problem 2

- [ ] **Carve the 4.4 ROM** (`scripts/extract-rom.sh`) → `rknand.ko`, its `modinfo`,
      the `nand-controller` DT node from the 4.4 `.dtb`, and `/proc/mtd`-era layout.
- [ ] **Disassemble `rknand.ko`** (armv7): recover the register map & sequences the blob
      pokes at `0x30030000` (command/addr/data regs, ECC config, timing, page/OOB size).
      `arm-linux-gnueabihf-objdump -d`, cross-ref MMIO offsets.
- [ ] **Compare against mainline** `drivers/mtd/nand/raw/rockchip-nand-controller.c` and
      `rk-nfc` bindings — determine whether this controller is a supported NFC variant
      (then it's a DT + compatible-string job) or a different block (then a new driver).
- [ ] **Dump the vendor NAND layout** from the running box (once we can read it) —
      idblock/vendor/env/boot/rootfs partition offsets, ECC scheme, BBT.
- [ ] **Prototype** a minimal read-only MTD driver, validate against a known-good dump,
      then read-write.

## Multitool steP-nand — reference

Menu path (on a Multitool SD): **"Burn Armbian image via steP-nand"** → device `rknand0`
→ pick image. It writes the Rockchip idbloader/miniloader to the NAND boot area and lays
the OS out for boot. The Multitool rootfs (extract via `scripts/extract-rom.sh` too) is
the authoritative source for the exact dd offsets and the loader blobs. Always
**"Backup flash"** first — this unit had **no vendor Android backup** taken before the
NAND was overwritten, so the original firmware is already lost.

## Fallback (documented reality)

If a mainline NAND driver proves infeasible, the supported configuration for NAND boxes
is the **legacy 4.4 image installed to NAND** (has `rknand`), while **6.6 runs from SD**.
This repo's Track A (WiFi on 6.6) is independent and still worth shipping.
