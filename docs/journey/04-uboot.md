# 04 — Getting the vendor U-Boot to boot us

With NAND writes working, the remaining question was the boot chain. Mainline
U-Boot is not an option here, for two independent reasons:

- Its TPL/SPL has no rk322x NAND support, so it cannot load itself from flash.
- Even the recent `rockchip_nfc.c` raw-MTD driver would not help: this flash is
  managed by the vendor FTL, not raw MTD/UBI, so mainline would see no partitions
  and no filesystem.

So the first two stages stay vendor — DDR init blob and miniloader — and vendor
U-Boot 2017.09 sits above them, because `CONFIG_RKNAND=y` links **the same FTL
blob** that our kernel module uses. U-Boot and Linux therefore agree on the
layout for free.

## The idblock

The BootROM does not load a raw binary. It wants an **idblock**, and the format is
easy to get wrong because of one misleading detail: the magic everyone quotes,
`0xFCDC8C3B`, is simply the **RC4-encrypted form of U-Boot's `RK_MAGIC`
(`0x0FF0AA55`)**. Only the 512-byte header is encrypted; the payload after it is
stored in the clear.

Rather than guess, the layout was confirmed against a known-good reference: the
box's own Armbian SD card at LBA 64, which boots this exact BootROM. RC4-decoding
its first sector gives

```
magic 0x0ff0aa55   disable_rc4 1   init_offset 4
init_size 20 blocks   init_boot_size 1044 blocks
```

so the image is

```
0      512 B    header0_info, RC4-encrypted   (reads as 3b 8c dc fc)
512   1536 B    zero padding to init_offset*512
2048     N B    DDR init blob, PLAINTEXT, starts with "RK32"
...      M B    next-stage loader, PLAINTEXT
```

Two things about the rkbin `BOOT` container are easy to get backwards:

- **Use `FlashData` + `FlashBoot`, not `code471` + `code472`.** The 471/472 pair
  is ddr + *usbplug*, which is the USB/maskrom path. The flash path is ddr +
  *miniloader*.
- **Payloads are RC4'd in independent 512-byte blocks**, not as one buffer. Proved
  two ways: per-block decoding of the container's DDR blob reproduces 79.9 % of
  the SD's DDR blob (same blob family, different version) versus 5.5 % for
  whole-buffer, and per-block decoding of `FlashBoot` yields real strings
  (`efuse hash data: `, its build date).

[`tools/mk-idblock.py`](../../tools/mk-idblock.py) builds and inspects these.

## Writing the idblock

It cannot be `dd`'d into place: the BootROM wants it in reserved physical blocks
with 70-bit ECC, the randomizer off, and a `"NINF"` OOB tag. The FTL does all of
that itself, through a hook most people never notice — `FtlWrite` calls the
exported `write_loader_lba(lba, nsec, buf)`, which

1. arms a 256000-byte capture when a write lands on **LBA 64** whose first word is
   `0xFCDC8C3B` (or `0x534E4B52`, "RKNS"),
2. accumulates sequential sectors,
3. commits via `write_idblock` on any write at **LBA ≥ 564**.

It trims trailing zero words, so pad with `0x00`:

```sh
dd if=idblock.bin of=/dev/rknand0 bs=512 seek=64  oflag=direct conv=notrunc
dd if=/dev/zero   of=/dev/rknand0 bs=512 seek=564 count=1 oflag=direct conv=notrunc
```

A good commit logs `write_idblock totle_sec d8 d8`, a run of `prog page:` lines
(the first carrying `fcdc8c3b`) and a matching run of `read page:` verifications.

Note the `/dev/rknand_sys_storage` ioctl route documented from the 4.4 blob does
**not** exist on FTL 5.0.63 — see [article 02](02-porting-the-ftl.md).

## The partition table, and a sector that does double duty

The miniloader and U-Boot find partitions through an RK `parameter` blob, and
`disk/part_rkparm.c:136` has a detail that matters:

```c
if (dev_desc->if_type != IF_TYPE_RKNAND)
        offset = RK_PARAM_OFFSET;   /* 0x2000 sectors */
ret = blk_dread(dev_desc, offset, ...);
```

On SD and eMMC the parameter lives at sector `0x2000`; **on an rknand device it is
read from sector 0.**

Linux, meanwhile, does not parse RK parameters at all — it would see one
undivided `/dev/rknand0`. The two can coexist in the same sector: the parameter
blob is 301 bytes and an MBR lives at bytes 446–511. So sector 0 carries both,
verified in place at once (`PARM` tag and the `55aa` signature), giving
`/dev/rknand0p1..p4` to Linux and a valid parameter to U-Boot.

## Four defects in vendor U-Boot 2017.09

Each of these cost a build-and-flash cycle. They are listed in the order they
were hit.

### 1. `CONFIG_SYS_MALLOC_F_LEN = 0x1000` is too small — and it is fatal

U-Boot died before doing anything:

```
DRAM:  Calloc ddr memory failed
Bidram Error: Can't get dram banks
initcall ... err=-12
```

4 KB of pre-relocation malloc is not enough once `initf_dm` has bound the
pre-relocation drivers, so the `calloc` in `arch/arm/mach-rockchip/param.c:316`
returns NULL. `initf_malloc` runs at `common/board_f.c:833`, well before
`dram_init` at `:890` — the pool exists, it is simply exhausted. **Raise it to
`0x8000`.**

### 2. `CONFIG_BAUDRATE` alone does not set the console baud rate

The console stayed at 1.5 Mbaud no matter what, because
`param_parse_pre_serial()` copies the *miniloader's* baud rate out of the atags:

```c
gd->serial.baudrate = t->u.serial.baudrate;   /* 1500000 */
gd->baudrate        = CONFIG_BAUDRATE;        /* 115200, and unused */
```

USB-serial adapters garble 1.5 Mbaud badly — characters drop, and the log becomes
unreadable exactly when it matters. Set `CONFIG_BAUDRATE=115200` **and**
`# CONFIG_ROCKCHIP_PRELOADER_SERIAL is not set`, which takes the `else` branch and
uses `CONFIG_BAUDRATE` with `CONFIG_DEBUG_UART_BASE`.

Also set `CONFIG_BOOTDELAY=3`; the vendor default of 0 leaves no way to interrupt.

### 3. `distro_bootcmd` cannot reach the boot partition

Three independent problems, all visible in one clean log:

```
## Error: "rknand_boot" not defined
starting USB...  2 USB Device(s) found
usb - USB sub-system / Usage: usb start ...
```

- **`rknand_boot` is never defined** — Rockchip did not add
  `BOOTENV_SHARED_RKNAND` to the `BOOTENV` macro, so the variable the rknand boot
  target runs does not exist.
- **`part_rkparm.c:214` hardcodes `info->bootable = 0`**, so
  `part list ${devtype} ${devnum} -bootable devplist` is always empty and
  `scan_dev_for_boot_part` falls back to scanning partition 1 only — the raw uboot
  partition, which has no filesystem.
- **`CONFIG_USB_STORAGE` is off**, so `usb dev` and `usb part` only print the
  command usage. U-Boot enumerates USB devices but has no block layer for them.

The second one looks like the obvious thing to patch. **Do not.** A build that set
`info->bootable = !strcmp(p->name, "boot")` made U-Boot reset right after
`Using default environment`, before `distro_bootcmd` ever ran. The cause was never
established, and the whole area is avoidable: override `CONFIG_BOOTCOMMAND` in
`include/configs/evb_rk3229.h` to call sysboot directly.

```c
#define CONFIG_BOOTCOMMAND \
	"sysboot rknand 0:3 any ${scriptaddr} /extlinux/extlinux.conf; " \
	RKIMG_BOOTCOMMAND
```

`sysboot` returns on failure, so the stock Rockchip sequence still runs
afterwards — including the mmc targets, which keep the SD card working as a
recovery path. `sysboot` itself needs no config: it is compiled unconditionally
into `cmd/pxe.c`, and `CONFIG_CMD_PXE=y` is already set.

### 4. `rknand dev` does not exist

`cmd/rknand.c` implements `scan`, `info`, `device` and `part` — there is no `dev`.
A guard written as

```
if rknand dev 0; then sysboot ... ; fi
```

fails silently and skips the command it was meant to protect. This cost a full
build cycle. The working version has no guard at all.

## The pieces the kernel needs

- **The DTB must already contain the vendor NAND node**, because U-Boot does not
  apply Armbian's `user_overlays`:
  `fdtoverlay -i rk322x-box.dtb -o rk322x-box-nand.dtb nand-vendor.dtbo`
- **The initrd must carry `rknand.ko`**, since the rootfs lives behind the FTL.
  Build it against a *copy* of the initramfs config so the SD's own initrd — the
  thing that gets you back in when a test fails — is left alone:

```sh
cp -a /etc/initramfs-tools ./ic && echo rknand >> ./ic/modules
mkinitramfs -d ./ic -o initrd-nand.img $(uname -r)
```

## What a good boot looks like

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

Harmless noise on the way: `No valid DTB, ret=-22` is U-Boot looking for a kernel
DTB in an Android-style `resource.img` we do not use, and
`Could not find kernel partition` is `bootrkp` looking for a partition named
`kernel` while ours is named `boot`.

## One cosmetic regression

The RJ45 LED lights much later than it does from SD. Armbian's mainline U-Boot has
a correct board DTB and brings the GMAC and PHY up early; vendor U-Boot prints
`No ethernet found` because its DTB is `rk3229-evb`, which declares
`phy-mode = "rgmii"` with an external GMAC clock — while this board has the
integrated RMII PHY (`rk_gmac-dwmac: init for RMII`, `integrated PHY? (yes)`).

Ethernet itself is unaffected. The LED was fixed on the Linux side instead — see
[article 05](05-pitfalls-and-lessons.md).
