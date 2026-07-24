# Track B — live NAND probe results (2026-07-24)

Ran the mainline Rockchip NFC driver against the box's NAND **on the running 6.6 kernel**,
without a kernel rebuild: built the MTD stack as out-of-tree modules against the pinned
KDIR and enabled the controller node with a **runtime DT overlay** (configfs).

## What works

- **Controller is driven by mainline.** `rockchip-nand-controller` (rk_nfc) binds to
  `nand-controller@30030000` and talks to the chip.
- **Chip identified:**
  ```
  Manufacturer 0x2C (Micron), Chip ID 0x64
  Micron MT29F64G08CBABAWP — 8192 MiB, MLC
  page 8192 B, OOB 744 B, erase block 2048 KiB
  is-boot-medium
  ```
- Node enabled live via overlay → platform device `30030000.nand-controller` created,
  no reboot, no kernel patch. Modules: `bch mtd nandcore nand rockchip-nand-controller`
  (see `env/build-mtd-modules.sh`; `bch` built as an external module because Armbian ships
  `CONFIG_MTD=n`/`CONFIG_BCH=n`).

## What does NOT work — reading the existing install

The NAND is **not blank** (it holds the Armbian 6.6 image the owner wrote via Multitool
*steP-nand*), but the mainline raw-NAND driver **cannot decode it**:

| ecc-strength (step 1024) | fits 744 OOB? | result |
|---|---|---|
| 16 | yes | ECC errors across the chip, BBT write fails (-28) |
| 24 | yes | ~2054 ECC errors, half the chip marked bad, -28 |
| 40 | yes | ~2058 ECC errors, -28 |
| 60 | **no** (8×105 B > 744) | fails oobsize check (-22) before reading |
| 70 | yes | ~2058 ECC errors, -28 |

No fitting strength reads cleanly. This is the signature of Rockchip's **proprietary
`rknand` on-flash format** (its own FTL + ECC organisation), which Multitool writes — it
is *not* a raw-MTD layout, so the mainline raw-NAND path cannot interpret it regardless of
ECC strength.

## Consequences

- **Reading the current NAND install requires the vendor stack** (4.4 `rknand` / boot the
  Multitool SD) — mainline can confirm the chip is populated but can't mount it.
- **Using NAND from mainline** means erasing the rknand FTL and doing a **fresh raw-MTD
  install** (destroys the existing content), with an ECC strength that fits 744 B OOB
  (≤ ~48-bit over 1024). Boot-from-NAND then also needs the idbloader written in a layout
  the BootROM accepts.
- So mainline NAND on this box is **usable but not interoperable** with the vendor format;
  it's a fresh-install path, not a read-the-existing-install path.

## Reproduce

```bash
make -C env kdir-src                                   # KDIR pinned to 6.6.22
docker run --rm -v $PWD:/work -w /work rk322x-xbuild env/build-mtd-modules.sh
# then load bch,mtd,nandcore,nand,rockchip-nand-controller on the box and apply the
# status="okay" overlay to /nand-controller@30030000 (see scripts in git history)
```
