#!/usr/bin/env python3
"""Build a Rockchip idblock (BootROM boot block) from an rkbin BOOT container.

Why this exists
---------------
The RK322x BootROM boots from an "idblock": a 512-byte RC4-encrypted header
followed by the DDR init blob and the next-stage loader. rkbin's boot_merger
only emits the USB "BOOT" container (rk322x_loader_*.bin), which is a different
format -- it starts with the ASCII tag "BOOT" and is what rkdeveloptool streams
to a chip in maskrom mode. To boot the box from its NAND we need the idblock
form instead, because the vendor FTL's loader-write path only accepts a buffer
whose first sector already carries the encrypted magic.

Layout, confirmed against a known-good idblock
----------------------------------------------
The reference is the box's own Armbian SD card at LBA 64, which boots this exact
BootROM. Reading it back and RC4-decoding the first sector yields:

    magic 0x0ff0aa55   disable_rc4 1   init_offset 4
    init_size 20 blocks   init_boot_size 1044 blocks

so the on-flash image is:

    offset 0      512 B    header0_info, RC4-encrypted (reads as 3b 8c dc fc)
    offset 512    1536 B   zero padding up to init_offset * 512
    offset 2048   N B      DDR init blob, PLAINTEXT (starts with "RK32")
    offset ...    M B      next-stage loader, PLAINTEXT

The struct is U-Boot's `struct header0_info` (tools/rkcommon.c): u32 magic,
u8 reserved[4], u32 disable_rc4, u16 init_offset, u8 reserved1[492],
u16 init_size, u16 init_boot_size, u8 reserved2[2]. Sizes are in 512-byte
blocks. `disable_rc4 = 1` means the payload after the header is stored in the
clear -- which is what the working SD does, so we do the same.

Which blobs to take
-------------------
A BOOT container holds several payloads and it matters which pair is used:

    code471 / code472  = DDR init + usbplug   -> the USB/maskrom path
    FlashData / FlashBoot = DDR init + miniloader -> what gets written to flash

We want FlashData + FlashBoot. Payloads inside the container are RC4-encrypted
in independent 512-byte blocks (verified two ways: decoding per-block reproduces
79.9% of the SD's DDR blob, which is the same blob family at a different version,
versus 5.5% for whole-buffer RC4; and per-block decoding of FlashBoot yields real
strings such as "efuse hash data: " and its build date). We decrypt them and
store them plaintext, matching the SD reference.

Usage
-----
    python3 mk-idblock.py rk322x_loader_v1.10.256.bin -o idblock.bin
    python3 mk-idblock.py --inspect idblock.bin        # decode any idblock
"""
import argparse
import struct
import sys

RC4_KEY = bytes([124, 78, 3, 4, 85, 5, 9, 7, 45, 44, 123, 56, 23, 13, 23, 17])
RK_MAGIC = 0x0FF0AA55
BLK = 512
INIT_OFFSET = 4          # in blocks; payload starts at 2048 bytes


def rc4(data, key=RC4_KEY):
    """RC4 is its own inverse, so this both encrypts and decrypts."""
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for b in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def rc4_blocks(data, size=BLK):
    """RC4 each 512-byte block independently, restarting the key schedule."""
    return b"".join(rc4(data[i:i + size]) for i in range(0, len(data), size))


def parse_boot_container(path):
    """Return {name: plaintext_bytes} for every payload in an rkbin BOOT file.

    Entry layout is 57 bytes: u8 size, u32 type, u16 name[20] (UTF-16LE),
    u32 data_offset, u32 data_size, u32 data_delay.
    """
    d = open(path, "rb").read()
    if d[:4] != b"BOOT":
        raise SystemExit(f"{path}: not an rkbin BOOT container (tag {d[:4]!r})")

    payloads = {}
    off = 0x19
    for _ in range(3):                      # code471, code472, loader lists
        num, ent_off, ent_size = struct.unpack_from("<BIB", d, off)
        off += 6
        for i in range(num):
            e = ent_off + i * ent_size
            name = d[e + 5:e + 45].decode("utf-16-le").split("\0")[0]
            data_off, data_size, _delay = struct.unpack_from("<III", d, e + 45)
            payloads[name] = rc4_blocks(d[data_off:data_off + data_size])
    return payloads


def build_idblock(ddr, loader):
    for tag, blob in (("DDR init", ddr), ("loader", loader)):
        if len(blob) % BLK:
            raise SystemExit(f"{tag} blob is {len(blob)} bytes, not a multiple of {BLK}")

    header = bytearray(INIT_OFFSET * BLK)
    struct.pack_into("<I", header, 0, RK_MAGIC)
    struct.pack_into("<I", header, 8, 1)                 # disable_rc4
    struct.pack_into("<H", header, 12, INIT_OFFSET)
    struct.pack_into("<H", header, 506, len(ddr) // BLK)             # init_size
    struct.pack_into("<H", header, 508,
                     (len(ddr) + len(loader)) // BLK)                # init_boot_size

    # Only the first block is encrypted; the rest of the 2KB stays zero padding.
    return bytes(rc4(bytes(header[:BLK]))) + bytes(header[BLK:]) + ddr + loader


def inspect(path):
    raw = open(path, "rb").read()
    h = rc4(raw[:BLK])
    magic, disable_rc4, init_off = (struct.unpack_from("<I", h, 0)[0],
                                    struct.unpack_from("<I", h, 8)[0],
                                    struct.unpack_from("<H", h, 12)[0])
    init_size, init_boot = (struct.unpack_from("<H", h, 506)[0],
                            struct.unpack_from("<H", h, 508)[0])
    print(f"{path}  ({len(raw)} bytes = {len(raw)//BLK} blocks)")
    print(f"  first 4 bytes on media : {raw[:4].hex()}")
    print(f"  magic                  : 0x{magic:08x} "
          f"{'OK' if magic == RK_MAGIC else '** NOT RK_MAGIC **'}")
    print(f"  disable_rc4            : {disable_rc4}")
    print(f"  init_offset            : {init_off} blocks ({init_off*BLK} bytes)")
    print(f"  init_size              : {init_size} blocks ({init_size*BLK} bytes)")
    print(f"  init_boot_size         : {init_boot} blocks ({init_boot*BLK} bytes)")
    body = raw[init_off * BLK:]
    print(f"  payload tag            : {body[:4]!r} "
          f"{'(RK32 DDR blob)' if body[:4] == b'RK32' else ''}")
    loader = raw[(init_off + init_size) * BLK:]
    print(f"  loader first 16        : {loader[:16].hex()}")
    return magic == RK_MAGIC


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="rkbin BOOT container, or an idblock with --inspect")
    ap.add_argument("-o", "--output", help="where to write the idblock")
    ap.add_argument("--inspect", action="store_true",
                    help="decode and print an existing idblock instead of building one")
    ap.add_argument("--ddr", default="FlashData",
                    help="container payload to use as DDR init (default: FlashData)")
    ap.add_argument("--loader", default="FlashBoot",
                    help="container payload to use as next stage (default: FlashBoot)")
    args = ap.parse_args()

    if args.inspect:
        sys.exit(0 if inspect(args.image) else 1)
    if not args.output:
        ap.error("-o/--output is required when building")

    payloads = parse_boot_container(args.image)
    for want in (args.ddr, args.loader):
        if want not in payloads:
            raise SystemExit(f"{args.image}: no payload named {want!r}; "
                             f"found {sorted(payloads)}")

    ddr, loader = payloads[args.ddr], payloads[args.loader]
    print(f"{args.ddr:>10}: {len(ddr)} bytes, tag {ddr[:4]!r}")
    print(f"{args.loader:>10}: {len(loader)} bytes, tag {loader[:4]!r}")

    img = build_idblock(ddr, loader)
    with open(args.output, "wb") as f:
        f.write(img)
    print(f"\nwrote {args.output} ({len(img)} bytes = {len(img)//BLK} blocks)\n")
    inspect(args.output)


if __name__ == "__main__":
    main()
