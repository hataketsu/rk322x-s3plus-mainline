#!/usr/bin/env python3
"""Dump the RK322x NFC registers right after a known-good mainline page write.

The vendor FTL's program leaves the cells erased even though the controller
reports the transfer complete, so we need the register state of a write that
provably works to diff against.  Run this on the box with the mainline stack
loaded (nand-en overlay + rockchip-nand-controller):

    python3 nfc-regdump.py [block]

It erases the block, writes one page of a recognisable pattern through
/dev/mtd0, dumps the NFC registers while they still hold that write's state,
and finally reads the page back so we know the write really landed.
"""
import fcntl
import mmap
import os
import struct
import sys

MTD = "/dev/mtd0"
NFC_PHYS = 0x30030000
ERASE = 2 << 20
PAGE = 8192
MEMERASE = 0x40084D02      # _IOW('M', 2, erase_info_user{u32 start, u32 length})

REGS = [
    (0x00, "FMCTL"), (0x04, "FMWAIT"), (0x08, "FLCTL"), (0x0C, "BCHCTL"),
    (0x10, "DMA_CFG"), (0x14, "DMA_DATA_BUF"), (0x18, "DMA_OOB_BUF"),
    (0x1C, "DMA_ST"), (0x20, "BCH_ST"), (0x150, "RANDMZ_CFG"),
    (0x158, "0x158"), (0x160, "VERSION"), (0x16C, "INT_EN"),
    (0x170, "INT_CLR"), (0x174, "INT_ST"),
]


def dump(tag):
    f = os.open("/dev/mem", os.O_RDONLY)
    m = mmap.mmap(f, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=NFC_PHYS)
    print(f"--- NFC registers {tag} ---")
    for off, name in REGS:
        print(f"  {name:<13} 0x{off:03x} = 0x{struct.unpack('<I', m[off:off+4])[0]:08x}")
    m.close()
    os.close(f)


def main():
    blk = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    off = blk * ERASE
    pattern = bytes(range(256)) * (PAGE // 256)

    fd = os.open(MTD, os.O_RDWR)
    fcntl.ioctl(fd, MEMERASE, struct.pack("<II", off, ERASE))
    print(f"erased block {blk} @ 0x{off:x}")

    os.lseek(fd, off, os.SEEK_SET)
    n = os.write(fd, pattern)
    print(f"wrote {n} bytes")
    dump("right after a successful mainline write")

    back = os.pread(fd, PAGE, off)
    print("readback matches:", back == pattern, "first bytes", back[:8].hex())
    os.close(fd)


if __name__ == "__main__":
    main()
