# Track B — RK322x NAND via the mainline NFC driver (NOT reverse-engineering)

**Key finding:** this box's NAND controller is the **open Rockchip NFC**, already
supported by mainline — no closed `rknand` blob or disassembly needed.

```
DT node   : nand-controller@30030000
compatible: "rockchip,rk3228-nfc", "rockchip,rk2928-nfc"
mainline  : drivers/mtd/nand/raw/rockchip-nand-controller.c of_match has
            { .compatible = "rockchip,rk2928-nfc", .data = &nfc_v6_cfg }  ✓ matches
```

So Track B is a **kernel-config + device-tree** job, not a driver-writing job.

## Blockers (both config, not code)

1. Armbian's rk322x current kernel has **`# CONFIG_MTD is not set`** — the whole MTD
   subsystem is off, so there's nothing for a rockchip-nfc module to attach to.
2. The DT node is **`status = disabled`**.

## Plan

- [ ] Kernel config fragment (rebuild kernel — reuse the env/ cross toolchain + KDIR):
      ```
      CONFIG_MTD=y
      CONFIG_MTD_BLKDEVS=y
      CONFIG_MTD_BLOCK=y
      CONFIG_MTD_RAW_NAND=y
      CONFIG_MTD_NAND_ROCKCHIP=y
      ```
- [ ] Enable the DT node: overlay setting `nand-controller@30030000 { status = "okay"; }`
      (node already carries reg/clocks/assigned-clocks/interrupts + a `nand@0` child;
      may need `nand-ecc-*` tuning on the child).
- [ ] Boot the new kernel from SD, verify `/proc/mtd` + `/dev/mtd*` + the NFC probe in
      dmesg; dump the existing vendor NAND layout.
- [ ] Partition/format via mainline MTD; install rootfs.
- [ ] Boot-from-NAND: write vendor **idbloader + miniloader** (steP-nand, blobs from
      Multitool `bsp/`) to the NAND boot area — mainline u-boot NAND support is **not**
      required (BootROM + vendor miniloader read NAND; see `docs/nand-boot.md`).

## Caveat

The driver *binding* is confirmed by of_match; what's not yet proven is whether the
vendor's **existing on-NAND ECC/layout** is readable by the mainline NFC driver (vendor
may have used a different ECC scheme). For a **fresh** erase+write install via the
mainline driver this is a non-issue; only reading pre-existing vendor data might mismatch.
