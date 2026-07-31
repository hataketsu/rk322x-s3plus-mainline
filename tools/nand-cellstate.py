#!/usr/bin/env python3
"""Decide what the vendor FTL actually programmed into the NAND cells.

Run on the box AFTER booting with the mainline stack (nand-en overlay +
rockchip-nand-controller with the randomizer patches).  It reads the first page
of a range of erase blocks through /dev/mtd0 (descrambling + 40-bit BCH applied
by the driver) and classifies each block:

  ERASED   - reads as 0xFF with no ECC complaint, i.e. the cells are blank and
             the vendor FTL programmed NOTHING (its PROGRAM was a no-op).
  RAW-FF   - read fails / returns the descrambled-blank signature 2c99d86e...,
             same conclusion as ERASED for our purposes.
  FF-PROG  - reads as clean 0xFF *with* valid ECC: the FTL really did program a
             page, but the data it handed the NFC was all-0xFF (stale/never
             filled DMA buffer).
  DATA     - anything else; print the first bytes so we can see whether it is
             the a5a5a5a5 / 5a5a5a5a test pattern FlashMakeFactorBbt writes.

Usage:  python3 nand-cellstate.py [first_block] [count]
"""
import os
import sys

MTD = "/dev/mtd0"
ERASE = 2 << 20          # 2 MiB erase block
PAGE = 8192
BLANK_SIG = bytes.fromhex("2c99d86e7138bdde")   # raw 0xFF seen through the descrambler


def classify(buf):
    if buf is None:
        return "READ-ERR", b""
    head = buf[:8]
    if buf == b"\xff" * len(buf):
        return "FF-PROG/ERASED", head
    if head == BLANK_SIG:
        return "RAW-FF", head
    return "DATA", head


def main():
    first = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    fd = os.open(MTD, os.O_RDONLY)
    for blk in range(first, first + count):
        try:
            buf = os.pread(fd, PAGE, blk * ERASE)
        except OSError as e:
            print(f"block {blk:5d}  READ-ERR  {e}")
            continue
        kind, head = classify(buf)
        print(f"block {blk:5d}  {kind:15s} {head.hex()}")
    os.close(fd)


if __name__ == "__main__":
    main()
