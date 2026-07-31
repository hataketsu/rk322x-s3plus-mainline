# Booting the S3 Plus from its NAND

The whole chain is written to NAND and waiting for its first boot test. Nothing
here can brick the box — see [Why this is safe](#why-this-is-safe).

> Earlier revisions of this file planned a from-scratch mainline NAND driver and
> concluded 6.x-on-NAND was infeasible. Both are obsolete: the vendor FTL now runs
> under 6.6 (`drivers/nand-rk322x/rknand-port/`), and the boot chain below is real.

## The chain

```
BootROM
  -> idblock            (vendor DDR init + miniloader, in reserved physical blocks)
  -> miniloader         (reads the FTL, finds partitions via `parameter`)
  -> trust.img + uboot.img
  -> vendor U-Boot 2017.09   <- serial console, 3-second countdown
  -> extlinux            (/extlinux/extlinux.conf on the boot partition)
  -> mainline 6.6 kernel + rk322x-box DTB + initrd carrying rknand.ko
  -> rootfs on NAND
```

Mainline U-Boot cannot start this: its TPL/SPL has no rk322x NAND support, so the
first two stages stay the vendor DDR blob and miniloader. Everything from U-Boot
onwards is ours.

## Why this is safe

The BootROM prefers the SD card. The owner confirmed this on this exact box: it
used to have a complete, valid Android ROM on NAND, and inserting an SD still
booted from the SD every time. So:

- **SD inserted** — boots Armbian from SD, whatever is on NAND.
- **SD removed** — the NAND chain is tried.

Testing means pulling the SD; undoing a failed test means putting it back. The
board also has a reset/recovery button, and `/boot/boot.cmd` shows the SD U-Boot
can force MASKROM mode by writing a magic to GRF `os_reg[0]`.

## Layout

`parameter` lives at **LBA 0** on an rknand FTL device, not at 0x2000 as on
SD/eMMC — see `disk/part_rkparm.c:136`. The idblock is not in this logical space
at all; the FTL keeps it in reserved physical blocks.

| # | Name      | Start LBA | Size    | Contents |
|---|-----------|-----------|---------|----------|
| — | parameter | 0         | 1 sector| RK partition table (plus a hybrid MBR, below) |
| 1 | uboot     | 0x2000    | 0x2000  | `uboot.img` ("LOADER  ") |
| 2 | trust     | 0x4000    | 0x2000  | `trust.img` ("TOS     ") |
| 3 | boot      | 0x6000    | 0x20000 | ext4: kernel, initrd, DTB, `extlinux/` |
| 4 | rootfs    | 0x26000   | rest    | ext4: Armbian rootfs |

### The hybrid sector 0

Linux does not parse the RK `parameter`, so it would otherwise see one undivided
`/dev/rknand0`. The `parameter` blob is only 301 bytes and an MBR lives at bytes
446-511, so the two do not overlap and sector 0 carries both: the miniloader and
U-Boot read the `parameter`, Linux reads the MBR and gives us `/dev/rknand0p1`..`p4`.
Verified with both in place at once — the `PARM` tag and the `55aa` signature.

If the miniloader ever turns out to dislike the MBR signature, zeroing bytes
446-511 of sector 0 removes it without touching the `parameter`.

## Building the pieces

```sh
# idblock, from the rkbin BOOT container (uses FlashData + FlashBoot, not 471/472)
python3 tools/mk-idblock.py rk322x_loader_v1.10.256.bin -o idblock.bin
python3 tools/mk-idblock.py --inspect idblock.bin

# partition table
python3 tools/mk-rkparam.py -o parameter.bin
```

U-Boot needs two changes to `configs/rk322x_defconfig`, both of which matter for
recovery rather than for booting:

- `CONFIG_BAUDRATE=115200` — the vendor default of 1500000 would make the serial
  console unreadable with the rest of our tooling.
- `CONFIG_BOOTDELAY=3` — the vendor default of 0 leaves no way to interrupt
  U-Boot and fix things by hand.

`sysboot` (extlinux) needs no config: it is compiled unconditionally into
`cmd/pxe.c`, and `CONFIG_CMD_PXE=y` is already set. Then `./make.sh rk322x`
produces `uboot.img`, `trust.img` and `rk322x_loader_v1.10.256.bin`.

## Writing to NAND

Load the vendor FTL first (`user_overlays=nand-vendor`, then `insmod rknand.ko`)
so `/dev/rknand0` exists.

```sh
dd if=parameter.bin of=/dev/rknand0 bs=512 seek=0     conv=notrunc
dd if=uboot.img     of=/dev/rknand0 bs=512 seek=8192  conv=notrunc
dd if=trust.img     of=/dev/rknand0 bs=512 seek=16384 conv=notrunc
```

The idblock is different: it cannot be placed by hand, because the BootROM wants
it in reserved physical blocks with 70-bit ECC, the randomizer off and a "NINF"
OOB tag. The FTL does all of that itself. `FtlWrite` calls the exported
`write_loader_lba(lba, nsec, buf)`, which arms a capture when a write lands on
**LBA 64** with `0xFCDC8C3B` as its first word, accumulates sequential sectors,
and commits through `write_idblock` on any write at **LBA >= 564**. It trims
trailing zero words, so pad with `0x00`:

```sh
dd if=idblock.bin of=/dev/rknand0 bs=512 seek=64  oflag=direct conv=notrunc
dd if=/dev/zero   of=/dev/rknand0 bs=512 seek=564 count=1 oflag=direct conv=notrunc
```

A successful commit logs `write_idblock totle_sec d8 d8`, then a run of
`prog page:` lines (the first carrying `fcdc8c3b`) and a matching run of
`read page:` verifications.

The `/dev/rknand_sys_storage` ioctl route does **not** work on FTL 5.0.63:
`rknand_sys_storage_ioctl` accepts only cmd `0x40047254` (flush) and returns
`-EINVAL` for everything else. The `WRITE_SECTOR`/`END_WRITE` ioctls that
`tools/rknand-write-loader.py` implements exist only in the older 4.4 blob.

## The DTB and the initrd

U-Boot does not apply Armbian's `user_overlays`, so the DTB handed to the kernel
must already contain the vendor NAND node:

```sh
fdtoverlay -i /boot/dtb-*/rk322x-box.dtb -o rk322x-box-nand.dtb nand-vendor.dtbo
```

The rootfs lives on NAND but needs `rknand.ko` to be mounted, so the module has to
be in the initrd. Build it against a *copy* of the initramfs config, so the SD's
own initrd — the thing that gets us back in when a test fails — is left alone:

```sh
cp rknand.ko /lib/modules/$(uname -r)/extra/ && depmod -a
cp -a /etc/initramfs-tools ./ic && echo rknand >> ./ic/modules
mkinitramfs -d ./ic -o initrd-nand.img $(uname -r)
```

## Testing

1. Start the UART capture on the host: `bash tools/uart-log.sh`.
2. Power the box off, remove the SD, power on.
3. Watch `uart.log`.

| What you see | Meaning |
|---|---|
| nothing at all | BootROM found no valid idblock and fell through to maskrom |
| garbage, then a U-Boot banner | normal — the miniloader prints at its own baud rate |
| U-Boot banner + 3s countdown | idblock, miniloader, trust and U-Boot are all good |
| `Retrieving file: /extlinux/extlinux.conf` | U-Boot is reading ext4 on the FTL |
| kernel log, then a rootfs mount | the whole chain works |

Reaching extlinux should need no typing. `RKIMG_BOOTCOMMAND`
(`include/configs/rockchip-common.h:174`) runs `boot_android; boot_fit; bootrkp;
run distro_bootcmd`. The first three look for Android/FIT/RK images and will fail
against our ext4 boot partition, and then `distro_bootcmd` takes over —
`CONFIG_CMD_RKNAND=y`, so `BOOT_TARGET_RKNAND` is compiled into
`BOOT_TARGET_DEVICES` and `scan_dev_for_boot_part` walks the rknand partitions
looking for exactly `/extlinux/extlinux.conf`. Expect three harmless failures
before it works.

Note `CONFIG_ENV_IS_NOWHERE=y`: the environment cannot be saved. Anything typed at
the prompt lasts for that boot only, so a permanent change to `bootcmd` means
rebuilding U-Boot.

Interrupt the countdown for a prompt. Useful from there:

```
rknand part            # does U-Boot see the partition table?
ext4ls rknand 0:3 /    # can it read the boot partition?
sysboot rknand 0:3 any ${scriptaddr} /extlinux/extlinux.conf
```

To get back to a known state at any point, put the SD back in.
