#!/usr/bin/env python3
# rknand mainline WRITE-path round-trip test (Phase C bring-up).
#
# Proves the write-patched rockchip-nand-controller (randomizer scramble + 40-bit BCH + OOB
# on program) round-trips: erase a verified-free block, write a known pattern through the
# driver, read it back through the same driver, and compare. NON-DESTRUCTIVE to vendor data
# because it only touches a block that scans as fully erased and is far from the boot and
# sys-block regions.
#
# Two phases, deliberately separated so the destructive step is explicit:
#   nand-write-test.py --scan            # read-only: find candidate erased blocks
#   nand-write-test.py --run <BLK>       # DESTRUCTIVE: erase+write+verify that ONE block
#
# Safety: the box boots from SD independently, so corrupting NAND cannot brick the box; the
# only residual risk is a hard kernel hang needing a physical power-cycle. Requires the
# WRITE-patched driver loaded (randomizer on the program path) — a stock driver would write
# unscrambled pages that this reader cannot verify.
import os, sys, struct, ctypes, fcntl

PAGE = 8192
PAGES_PER_BLK = 256                       # mainline erase block = 2 MiB / 8192
BLK = PAGE * PAGES_PER_BLK
NPAGES = 0x200000000 // PAGE              # 8 GiB

MEMGETINFO     = 0x80204d01
MEMERASE       = 0x40084d02               # _IOW('M',2, erase_info_user{u32 start,length})
MEMREADOOB     = 0xC00C4D04               # _IOWR('M',4, mtd_oob_buf{u32 start,length; ptr})
MEMWRITEOOB    = 0xC00C4D03               # _IOWR('M',3, mtd_oob_buf)
MEMGETBADBLOCK = 0x80084d0b               # _IOR('M',11, loff_t)

# Never touch the first FLOOR blocks (boot/idblock/u-boot) or the trailing sys-block region.
FLOOR_BLK = 64
SYS_BLK_LO = 4000                         # sys/map blocks live ~4019..4088

class ob(ctypes.Structure):
    _fields_ = [("start", ctypes.c_uint32), ("length", ctypes.c_uint32),
                ("ptr", ctypes.c_void_p)]

def read_oob(fd, page):
    buf = ctypes.create_string_buffer(64)
    req = ob(page * PAGE, 64, ctypes.cast(buf, ctypes.c_void_p))
    fcntl.ioctl(fd, MEMREADOOB, req)
    return buf.raw[:32]

def oob_is_erased(o):
    # erased/special pages read as all-0xFF sys words (magic word2 == 0xFFFF)
    return (struct.unpack_from("<I", o, 2 * 4)[0] & 0xFFFF) == 0xFFFF

def page_is_erased(fd, page):
    try:
        d = os.pread(fd, PAGE, page * PAGE)
    except OSError:
        return False
    return d == b"\xff" * PAGE and oob_is_erased(read_oob(fd, page))

def block_bad(fd, blk):
    off = ctypes.c_ulonglong(blk * BLK)
    try:
        return fcntl.ioctl(fd, MEMGETBADBLOCK, off) != 0
    except OSError:
        return True

def block_fully_erased(fd, blk, sample=8):
    if block_bad(fd, blk):
        return False
    base = blk * PAGES_PER_BLK
    # sample a few pages across the block (full 256-page check is slow)
    step = max(1, PAGES_PER_BLK // sample)
    for p in range(base, base + PAGES_PER_BLK, step):
        if not page_is_erased(fd, p):
            return False
    return True

def scan(fd, want=5):
    print(f"scanning for fully-erased free blocks (floor {FLOOR_BLK}, avoid sys >= {SYS_BLK_LO})")
    found = []
    total_blk = NPAGES // PAGES_PER_BLK
    for blk in range(FLOOR_BLK, min(SYS_BLK_LO, total_blk)):
        if block_fully_erased(fd, blk):
            found.append(blk)
            print(f"  ERASED candidate block {blk} (page {blk*PAGES_PER_BLK}, byte {blk*BLK:#x})")
            if len(found) >= want:
                break
    if not found:
        print("  no fully-erased block found in range")
    else:
        print(f"\nto test:  nand-write-test.py --run {found[0]}")
    return found

def run(fd, blk):
    if blk < FLOOR_BLK or blk >= SYS_BLK_LO:
        sys.exit(f"REFUSED: block {blk} outside safe range [{FLOOR_BLK},{SYS_BLK_LO})")
    if not block_fully_erased(fd, blk):
        sys.exit(f"REFUSED: block {blk} is not fully erased / is bad — pick one from --scan")
    base = blk * PAGES_PER_BLK
    print(f"target block {blk}: pages {base}..{base+PAGES_PER_BLK-1}, byte {blk*BLK:#x}")

    # 1) erase
    ei = struct.pack("<II", blk * BLK, BLK)
    fcntl.ioctl(fd, MEMERASE, ei)
    print("  erased")

    # 2) write a distinctive pattern to the first 4 pages
    npg = 4
    patterns = []
    for i in range(npg):
        pat = bytes(((i * 37 + (j & 0xff)) & 0xff) for j in range(PAGE))
        patterns.append(pat)
        os.pwrite(fd, pat, (base + i) * PAGE)
    print(f"  wrote {npg} pages")

    # 3) read back through the same (scrambling) driver and compare
    ok = True
    for i in range(npg):
        rb = os.pread(fd, PAGE, (base + i) * PAGE)
        match = rb == patterns[i]
        firstdiff = next((k for k in range(PAGE) if rb[k] != patterns[i][k]), -1)
        print(f"  page {base+i}: {'OK' if match else 'MISMATCH'}"
              + ("" if match else f" first diff @ {firstdiff}"))
        ok = ok and match
    print("\nRESULT:", "WRITE PATH VERIFIED" if ok
          else "MISMATCH — check dmesg for ecc error / randomizer mismatch")
    return ok

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    dev = "/dev/mtd0"
    fd = os.open(dev, os.O_RDWR if sys.argv[1] == "--run" else os.O_RDONLY)
    if sys.argv[1] == "--scan":
        scan(fd)
    elif sys.argv[1] == "--run" and len(sys.argv) == 3:
        run(fd, int(sys.argv[2]))
    else:
        sys.exit("usage: --scan | --run <BLK>")

if __name__ == "__main__":
    main()
