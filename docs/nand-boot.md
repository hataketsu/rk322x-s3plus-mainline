# Booting the S3 Plus from its NAND

**This works.** With the SD card physically removed the box boots unattended into a
full Armbian userspace: `findmnt /` reports `/dev/rknand0p4`, `lsblk` shows only
`rknand0` (no `mmcblk0`), `systemctl is-system-running` says `running` with no
failed units, and both ethernet and the RTL8189ES wifi come up. Zero
`prog error` / `bad block` / `ecc err` lines in dmesg.

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

### U-Boot changes

Patches are in `drivers/nand-rk322x/uboot/`; apply them to the vendor tree and run
`./make.sh rk322x`, which produces `uboot.img`, `trust.img` and
`rk322x_loader_v1.10.256.bin`.

`configs/rk322x_defconfig`:

- **`CONFIG_SYS_MALLOC_F_LEN=0x1000` → `0x8000`.** This one is mandatory: with only
  4 KB of pre-relocation malloc, the `calloc` in `arch/arm/mach-rockchip/param.c:316`
  returns NULL once `initf_dm` has bound the pre-reloc drivers, and U-Boot dies with
  `DRAM: Calloc ddr memory failed` / `Bidram Error: Can't get dram banks` /
  initcall `err=-12`. (`initf_malloc` runs at `common/board_f.c:833`, well before
  `dram_init` at `:890` — the pool exists, it is simply exhausted.)
- **`CONFIG_BAUDRATE=1500000` → `115200`** and **`# CONFIG_ROCKCHIP_PRELOADER_SERIAL
  is not set`.** Both are needed together: `param_parse_pre_serial()` copies the
  miniloader's baud rate out of the atags and ignores `CONFIG_BAUDRATE`, so the
  console stays at 1.5 Mbaud unless the preloader-serial option is off. USB-serial
  adapters tend to garble 1.5 Mbaud, which makes the log useless exactly when it is
  needed.
- **`CONFIG_BOOTDELAY=0` → `3`**, so autoboot can be interrupted.

`include/configs/evb_rk3229.h` overrides `CONFIG_BOOTCOMMAND` to run

```
sysboot rknand 0:3 any ${scriptaddr} /extlinux/extlinux.conf
```

before the stock `RKIMG_BOOTCOMMAND`. `sysboot` returns on failure, so the vendor
sequence — including the mmc targets that keep the SD card usable — still runs
afterwards.

### Why distro_bootcmd cannot find the boot partition by itself

Three independent defects in this vendor U-Boot, worth knowing before trying to
"fix it properly":

1. `## Error: "rknand_boot" not defined` — Rockchip never added
   `BOOTENV_SHARED_RKNAND` to the `BOOTENV` macro, so the variable the rknand boot
   target runs does not exist.
2. `disk/part_rkparm.c:214` hardcodes `info->bootable = 0`, so
   `part list ${devtype} ${devnum} -bootable devplist` is always empty and
   `scan_dev_for_boot_part` falls back to scanning partition 1 only — the raw uboot
   partition, which has no filesystem. Do **not** patch this: a build that set
   `info->bootable = !strcmp(p->name, "boot")` made U-Boot reset right after
   `Using default environment`, before `distro_bootcmd` ever ran. The cause was
   never established; the `CONFIG_BOOTCOMMAND` override avoids the whole area.
3. `usb dev` / `usb part` only print the command usage — `CONFIG_USB_STORAGE` is not
   enabled, so U-Boot enumerates USB devices but has no block layer for them. USB
   boot would need a rebuild with it turned on.

Also note `rknand dev` does not exist: `cmd/rknand.c` implements only
`scan`, `info`, `device` and `part`. An `if rknand dev 0; then ...` guard fails
silently and skips whatever it guards.

`sysboot` itself needs no config — it is compiled unconditionally into `cmd/pxe.c`,
and `CONFIG_CMD_PXE=y` is already set.

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

A good boot looks like this — the early lines are the miniloader at its own fixed
1.5 Mbaud and will be garbage at 115200, which is expected:

```
U-Boot 2017.09 ... Model: Rockchip RK3229 Evaluation board
DRAM:  2 GiB
Bootdev(atags): rknand 0 / PartType: RKPARM
Retrieving file: /extlinux/extlinux.conf
Retrieving file: /initrd.img          14210052 bytes read
Retrieving file: /vmlinuz              9838504 bytes read
Retrieving file: /rk322x-box-nand.dtb    44507 bytes read
Starting kernel ...
OF: fdt: Machine model: Generic RK322x Tv Box board
rk_nand: rknand vendor storage init ok !
EXT4-fs (rknand0p4): mounted filesystem ... r/w
```

`RKIMG_BOOTCOMMAND` still runs after our `sysboot`, so on failure you get the
vendor sequence (`boot_android`, `boot_fit`, `bootrkp`, `distro_bootcmd`) and then
a `=>` prompt after the 3-second countdown. From there the same command works by
hand:

```
ext4ls rknand 0:3 /
sysboot rknand 0:3 any ${scriptaddr} /extlinux/extlinux.conf
```

Note `CONFIG_ENV_IS_NOWHERE=y`: the environment cannot be saved, so anything typed
at the prompt lasts for that boot only and a permanent change means rebuilding.

To get back to a known state at any point, put the SD back in — and if the box
still insists on NAND, short NAND pins 29+30 during power-on.

## Known cosmetic difference

The RJ45 LED lights up much earlier when booting from the SD card. That is a
U-Boot difference, not a fault: Armbian's mainline U-Boot has a correct board DTB
and brings the GMAC and PHY up early, whereas the vendor U-Boot prints
`No ethernet found` because its DTB is `rk3229-evb`, which declares
`phy-mode = "rgmii"` with an external GMAC clock. This box actually has the
integrated RMII PHY — the kernel reports `rk_gmac-dwmac 30200000.ethernet: init
for RMII` and `integrated PHY? (yes)`. Ethernet itself is unaffected (100 Mb/s,
full duplex); only U-Boot-stage networking, i.e. netboot, would need a patched
U-Boot DTS.

For the record, the NAND pinctrl is *not* to blame: `flash-*` occupies
GPIO1_D0..D7 and GPIO2_A0..A7, which does not overlap the gmac `rmii-pins` or
`phy-pins` groups at all.
