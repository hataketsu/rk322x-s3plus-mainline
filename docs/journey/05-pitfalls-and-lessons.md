# 05 — Pitfalls and lessons

The condensed version. Roughly ordered by how much time each one cost.

## Hardware and device tree

**A missing pinctrl group produces a symptom in the peripheral, not in the DT.**
`flash-wp` was absent from the NAND node's `pinctrl-0`, so WP# was never muxed and
the chip ignored every PROGRAM and ERASE while reading perfectly. Nothing logs
"you forgot a pin". Before trusting a node, list what exists:

```sh
ls /proc/device-tree/pinctrl/<controller>/
```

**Controller registers describe what the controller drives, not what the chip
receives.** `FMCTL` bit 8 (WP#) read back as 1 in both the broken and the fixed
configuration — identical values, opposite behaviour. If a register says the
hardware is fine and the hardware disagrees, suspect the pad, not the register.

**Reads working proves nothing about writes.** Data, address and control pins can
all be muxed correctly while one control signal is not.

**The same class of bug bites twice.** The RJ45 LEDs were dark because
`/ethernet@30200000` has no `pinctrl` at all, so the gmac `phy-pins` group
(GPIO2_B0 and GPIO2_B6, function 2) is never muxed. Harmless for traffic — the
PHY is inside the SoC, no RMII pin leaves the chip — but those two pins drive the
LEDs. Confirmed by poking `GRF GPIO2B_IOMUX` (`0x11000024`) at runtime and
watching the LEDs come on, then fixed with a DT overlay.

## Debugging method

**When two independent drivers fail identically, stop debugging drivers.** The
closed FTL and mainline `rockchip-nand-controller` both could not write the flash.
That test was available on day one and would have saved most of three days.

**Read the media through a second, independent path.** The FTL's own readback
reported `0xffffffff` and was believed for days. Reading the same blocks with the
mainline driver answered a better question — *has anything ever been written
here?* — and killed an entire class of theory at once, because a programmed page
would hold `scramble(0xFF)`, not raw `0xFF`.

**Prove causation, not correlation.** The fix arrived alongside a reboot, so it
was re-tested with one phandle as the only variable: without `flash-wp`, erase and
write both `-EIO`; with it, both fine; `FMCTL` identical in both. One table
settled it.

**Get the serial console working before anything else.** Every wrong theory in
[article 03](03-the-write-protect-bug.md) came from reasoning without evidence.
The first clean UART log named the failure in one line. On this box that meant
soldering a missing 0 Ω series resistor on the `ttyS2` TX footprint.

**A garbled log is worse than no log**, because it looks like data. Vendor U-Boot
runs at 1.5 Mbaud, USB-serial adapters garble it, and characters drop silently —
several early diagnoses were made from logs that were missing exactly the lines
that mattered. Fix the baud rate first.

**`systemd-analyze blame` is not a boot-time budget.** Removing a 5.4 s entry
(`systemd-random-seed`) changed the total by zero, because it ran in parallel.
Only `systemd-analyze critical-chain` predicts what removing something will save.

## Blobs and vendor code

**Identify which half of the blob actually runs.** Both `rk_zftl_arm32.o` and
`rk_ftlv5_arm32.o` are linked. `rk_ftl_init` enters through ZFTL, whose
`nand_flash_init` rejects this MLC part and falls through to FTL-v5's
`FlashInit`. Chasing the lowercase ZFTL functions is a detour; the CamelCase v5
half is what executes.

**Blob interfaces are version-dependent.** `rknand_sys_storage_ioctl` on FTL
5.0.63 accepts exactly one command (`0x40047254`, flush) and returns `-EINVAL`
for everything else. The `WRITE_SECTOR`/`END_WRITE` ioctls reverse engineered from
the 4.4 blob do not exist. Re-derive against the blob you are actually linking.

**Vendor numbers can be about a different thing than you assume.** U-Boot prints
`ECC:60`; that is `g_idb_ecc_bits`, for the idblock only. Data is 40-bit. Chasing
60 fails in a confusing way: 60-bit BCH needs 840 B of parity, more than this
chip's 744 B OOB, so the driver refuses to probe with `-EINVAL`.

**Blank-media paths in vendor code are often untested.** `FtlInit` on a blank
device only *prints* "no sys info" and returns success without provisioning;
`FtlLowFormat` called directly oopses on a NULL bitmap because it needs
`FlashMakeFactorBbt` first. On real hardware the miniloader provisions over USB,
so the kernel FTL never exercises that path. `FtlReInitForSDUpdata()` does the
sequence in the right order.

## Reverse engineering

**Verify your tools before trusting their output.** `nanddump -s <offset>`
silently ignores the offset on a `NAND_SKIP_BBTSCAN` driver and returns page 0
every time, producing a complete and entirely fictional map. Use the
`MEMREADOOB` ioctl for real seeking.

**Check field offsets against a known-good sample.** Mainline exposes the NFC sys
bytes *rotated* — step 0 at OOB offset `(steps-1)*4`, steps 1–7 at 0–24 — and the
FTL magic is at sys-word 2, not word 7 (word 7 is a constant footer on every
page). Both mistakes produce plausible-looking garbage.

**Use a working artefact as the format reference.** The idblock layout was not
guessed: the box's own SD card at LBA 64 boots this exact BootROM, so
RC4-decoding its first sector gave the authoritative header. The same reference
settled how RC4 is applied (per 512-byte block, not whole-buffer) by comparing
match rates: 79.9 % versus 5.5 %.

**Distinguish the USB path from the flash path.** In an rkbin `BOOT` container,
`code471`/`code472` are ddr + usbplug (maskrom over USB); `FlashData`/`FlashBoot`
are ddr + miniloader (what gets written to flash).

## U-Boot specifics

- `CONFIG_SYS_MALLOC_F_LEN=0x1000` is not enough once `initf_dm` runs; `dram_init`
  fails with `Calloc ddr memory failed` and `err=-12`. Use `0x8000`.
- `CONFIG_BAUDRATE` is overridden by `param_parse_pre_serial()`, which adopts the
  miniloader's baud from the atags. Also set
  `# CONFIG_ROCKCHIP_PRELOADER_SERIAL is not set`.
- The RK `parameter` lives at **sector 0** on rknand, not `0x2000`
  (`disk/part_rkparm.c:136`).
- `distro_bootcmd` cannot find an rknand boot partition: `rknand_boot` is
  undefined, `part_rkparm.c` hardcodes `bootable = 0`, and `CONFIG_USB_STORAGE` is
  off. Override `CONFIG_BOOTCOMMAND` instead of patching — a `part_rkparm.c` patch
  made U-Boot reset before `distro_bootcmd` ran.
- `rknand dev` does not exist (only `scan`, `info`, `device`, `part`). A guard
  using it fails silently.
- `CONFIG_ENV_IS_NOWHERE=y` — `saveenv` does nothing; permanent changes require a
  rebuild.

## Operational

**Know your escape hatch before you need it, and verify it.** Once a valid
idblock is on NAND the BootROM boots NAND and ignores the SD card — an early note
claiming the opposite, based on a recollection about an Android install, was
wrong and led to writing an idblock while believing it was risk-free. The escape
that actually works on this board is **shorting NAND pins 29+30 during
power-on**: the BootROM fails to read the flash and falls through to the card.

**Do not pull the SD card while the system is running from it.** It kills the
running rootfs mid-operation — `mmc0: card 0007 removed`, ext4 journal aborted,
remounted read-only — and it corrupted an in-progress rsync. Power off first.

**Stage the work so one test yields maximum information.** Writing the loader,
trust image and boot partition before the first boot attempt meant a single
SD-pull exercised the whole chain, and the UART log said exactly how far it got.

**Watch for tools that need files you excluded.** `mkinitramfs` fails with
`gzip compression (CONFIG_RD_GZIP) not supported by kernel` when
`/boot/config-$(uname -r)` is missing — which it is, if `/boot/*` was excluded
from the rootfs rsync. It is not a compression problem at all.

**Measure optimisations; some are negative.** Shrinking the initrd to 11.3 MB with
`MODULES=dep` and zstd made kernel time slightly *worse* (6.67 s → 7.01 s) — this
CPU decompresses zstd slowly enough to eat the gain. Reverted to the known state.
