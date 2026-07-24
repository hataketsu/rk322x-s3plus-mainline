# Track B — RK322x NAND controller MTD driver (WIP)

Target: a mainline-style MTD driver for `nand-controller@30030000` so kernel 6.6 can use
the onboard NAND (and ultimately boot from it via steP-nand).

**This is research, not a rebuild of the vendor blob** — the 4.4 `rknand.ko` is closed and
ABI-locked (see `docs/nand-boot.md`). Work here is driven by:

1. register map recovered from disassembling the 4.4 `rknand.ko`
   (`docs/reverse-engineering.md`),
2. diffing the 4.4 vs 6.6 `nand-controller` device-tree node,
3. checking whether mainline `rockchip-nand-controller.c` already covers this controller
   variant (then it may be a DT/compatible-string job, not a new driver).

## Current state

- [ ] extract + disassemble `rknand.ko`
- [ ] recover MMIO register map @ `0x30030000`
- [ ] decompile 4.4 `.dtb` NAND node, diff vs 6.6 live node
- [ ] decide: mainline NFC variant vs new driver
- [ ] read-only MTD prototype validated against a NAND dump
- [ ] read-write + partitions
- [ ] steP-nand boot integration (Multitool loaders)

Nothing here is loadable yet. See `docs/nand-boot.md` for the plan and the honest
feasibility assessment.
