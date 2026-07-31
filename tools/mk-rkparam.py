#!/usr/bin/env python3
"""Build a Rockchip `parameter` block: the partition table the miniloader and
vendor U-Boot use to find uboot.img, trust.img and the boot/rootfs partitions.

On-flash format (U-Boot `struct rkparm_param`, disk/part_rkparm.c:17):

    u32 tag       0x4D524150, i.e. the ASCII "PARM"
    u32 length    byte length of the text that follows
    char params[length]
    u32 crc       Rockchip CRC-32 over the text

Where it lives depends on the medium. disk/part_rkparm.c:136 reads:

    if (dev_desc->if_type != IF_TYPE_RKNAND)
            offset = RK_PARAM_OFFSET;      /* 0x2000 sectors */
    ret = blk_dread(dev_desc, offset, ...);

so on SD/eMMC the parameter sits at sector 0x2000, but **on an rknand FTL device
it is read from sector 0**. That is where this block must be written.

The text is a `key: value` list; the part that matters is the CMDLINE line, whose
`mtdparts=` list gives each partition as `size@start(name)` with both numbers in
hex 512-byte sectors. The parser (part_rkparm.c:57-104) looks for the literal
"mtdparts", splits on ':' then ',', accepts '-' as "rest of device", and stops at
a newline -- so the CMDLINE must be the last line and must end with one.

Rockchip's CRC-32 is not the zlib one: it uses polynomial 0x04C10DB7 MSB-first
with an initial value of 0 and no final inversion.

Usage:
    python3 mk-rkparam.py -o parameter.bin
    python3 mk-rkparam.py -o parameter.bin --rootfs-name linuxroot
    python3 mk-rkparam.py --inspect parameter.bin
"""
import argparse
import struct

TAG = 0x4D524150            # "PARM"
SECTOR = 512

# Classic RK322x layout, in 512-byte sectors. The first 4 MB are left alone: the
# idblock does not live in this logical space (the FTL keeps it in reserved
# physical blocks), but Rockchip images conventionally keep the area free.
DEFAULT_PARTS = [
    ("uboot",  0x00002000, 0x00002000),   # 4 MB @ 4 MB
    ("trust",  0x00004000, 0x00002000),   # 4 MB @ 8 MB
    ("boot",   0x00006000, 0x00020000),   # 64 MB @ 12 MB
    ("rootfs", 0x00026000, None),         # rest of the device
]


def rk_crc32(data):
    """Rockchip CRC-32: poly 0x04C10DB7, MSB-first, init 0, no final xor."""
    crc = 0
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C10DB7) & 0xFFFFFFFF if crc & 0x80000000 \
                else (crc << 1) & 0xFFFFFFFF
    return crc


def build_text(parts, cmdline_extra=""):
    mtd = ",".join(
        f"0x{size:08x}@0x{start:08x}({name})" if size is not None
        else f"-@0x{start:08x}({name})"
        for name, start, size in parts)
    lines = [
        "FIRMWARE_VER:1.0",
        "MACHINE_MODEL:rk3229",
        "MACHINE_ID:007",
        "MANUFACTURER:RK3229",
        "MAGIC: 0x5041524B",
        "ATAG: 0x00200800",
        "MACHINE: 3229",
        "CHECK_MASK: 0x80",
        "PWR_HLD: 0,0,A,0,1",
        f"CMDLINE:{cmdline_extra}mtdparts=rk29xxnand:{mtd}",
    ]
    return "\n".join(lines) + "\n"


def build(parts, cmdline_extra="", pad_to=SECTOR):
    text = build_text(parts, cmdline_extra).encode()
    blob = struct.pack("<II", TAG, len(text)) + text + struct.pack("<I", rk_crc32(text))
    if len(blob) > pad_to:
        raise SystemExit(f"parameter is {len(blob)} bytes, does not fit in {pad_to}")
    return blob + b"\0" * (pad_to - len(blob))


def inspect(path):
    raw = open(path, "rb").read()
    tag, length = struct.unpack_from("<II", raw, 0)
    text = raw[8:8 + length]
    crc, = struct.unpack_from("<I", raw, 8 + length)
    good = rk_crc32(text) == crc
    print(f"{path} ({len(raw)} bytes)")
    print(f"  tag    : 0x{tag:08x} {struct.pack('<I', tag)!r} "
          f"{'OK' if tag == TAG else '** not PARM **'}")
    print(f"  length : {length}")
    print(f"  crc    : 0x{crc:08x} {'OK' if good else f'** MISMATCH, computed 0x{rk_crc32(text):08x} **'}")
    print("  text:")
    for line in text.decode(errors="replace").rstrip("\n").split("\n"):
        print(f"    {line}")
    return tag == TAG and good


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output")
    ap.add_argument("--inspect", metavar="FILE")
    ap.add_argument("--cmdline-extra", default="",
                    help="text inserted before mtdparts= on the CMDLINE line")
    args = ap.parse_args()

    if args.inspect:
        raise SystemExit(0 if inspect(args.inspect) else 1)
    if not args.output:
        ap.error("-o/--output is required")

    blob = build(DEFAULT_PARTS, args.cmdline_extra)
    with open(args.output, "wb") as f:
        f.write(blob)
    print(f"wrote {args.output}\n")
    inspect(args.output)


if __name__ == "__main__":
    main()
