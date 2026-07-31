#!/usr/bin/env python3
# rknand-write-loader.py
#
# Write a Rockchip IDBlock ("loader") to a blank RK322x NAND from Linux user
# space, through the /dev/rknand_sys_storage misc char device exposed by the
# rknand FTL driver.
#
# ============================ PROVENANCE / STATUS ============================
# Every ioctl number and struct field below was derived by disassembling the
# vendor 4.4 kernel:
#   extracted/kernel-4.4/vmlinux-4.4.194.bin
#   (_text = 0xb0008000 ; file_off = VA - 0xb0008000)
# Symbols from extracted/kernel-4.4/System.map-4.4.194-rk322x.
#
#   rknand_sys_storage_ioctl : VA 0xb0970ae4
#   write_idblock            : VA 0xb0970238   (checks magic 0xFCDC8C3B)
#   write_loader_lba         : VA 0xb09708b8   (LBA-64 auto-capture path)
#   RK CRC32 table           : VA 0xb1228500   (poly 0x04C10DB7)
#
# CONFIRMED  = read straight from disassembly.
# INFERRED   = deduced (e.g. default idb block-index array); flagged inline.
#
# !!! READ THE THREE BIG CAVEATS AT THE BOTTOM BEFORE RUNNING. In particular:
#   (1) The input file must be an IDBLOCK image whose first 4 bytes are
#       3b 8c dc fc  (0xFCDC8C3B little-endian). The rk322x_loader_*.bin in
#       rkbin is a "BOOT" container (first bytes 42 4f 4f 54 = "BOOT") and is
#       NOT directly writable here -- it must be wrapped into an IDBlock first.
#   (2) Writing the IDBlock does NOT make rk_ftl_get_capacity() non-zero. That
#       needs an FTL low-level format (FtlLowFormat). See notes at bottom.
# ============================================================================

import ctypes, fcntl, struct, sys, argparse

DEV = "/dev/rknand_sys_storage"
SECTOR = 512
IDB_MAGIC = 0xFCDC8C3B                 # CONFIRMED: write_idblock @0xb0970308

# ---- _IOC encoding (generic Linux / ARM): dir[31:30] size[29:16] type[15:8] nr[7:0]
_IOC_NRBITS, _IOC_TYPEBITS, _IOC_SIZEBITS = 8, 8, 14
_IOC_NONE, _IOC_WRITE, _IOC_READ = 0, 1, 2
def _IOC(d, t, nr, sz):
    return (d << 30) | (sz << 16) | (ord(t) << 8) | nr
def _IOW(t, nr, sz):
    return _IOC(_IOC_WRITE, t, nr, sz)

# ---- CONFIRMED command numbers (literal pool of rknand_sys_storage_ioctl) ----
# All are _IOW('r', nr, u32); the 'size' field is always 4 (sizeof(int)), it is
# NOT the real payload size -- the handler ignores it and copies a fixed amount.
GET_FLASH_INFO = _IOW('r', 3,  4)      # 0x40047203  -> copy_to_user(arg, buf, 64)
READ_SECTOR    = _IOW('r', 4,  4)      # 0x40047204  -> read  {u32 lba;u32 n;} n<=8
WRITE_SECTOR   = _IOW('r', 5,  4)      # 0x40047205  -> accumulate into loader buf
END_WRITE      = _IOW('r', 0x52, 4)    # 0x40047252  -> write_idblock() commit
GET_CAP_R83    = _IOW('r', 0x53, 4)    # 0x40047253  -> copy_to_user(arg, &x, 4)

assert GET_FLASH_INFO == 0x40047203, hex(GET_FLASH_INFO)
assert READ_SECTOR    == 0x40047204, hex(READ_SECTOR)
assert WRITE_SECTOR   == 0x40047205, hex(WRITE_SECTOR)
assert END_WRITE      == 0x40047252, hex(END_WRITE)
assert GET_CAP_R83    == 0x40047253, hex(GET_CAP_R83)

# The WRITE_SECTOR handler does copy_from_user(kbuf, arg, 4096) unconditionally
# (0xb0970cd8: mov r2,#0x1000). So the userspace buffer we hand to the ioctl
# must be at least 4096 bytes. Layout it reads:
#   struct write_req { u32 dst_off; u32 len; u8 data[4088]; }  (CONFIRMED)
#   dst_off <= 0x3d800 (251904)   (0xb0970d44)   byte offset into the RAM idb buf
#   len     <= 0xff8   (4088)     (0xb0970d38)   bytes copied this call
#   -> memcpy(loader_buf + dst_off, data, len)   (0xb0970d58)
WRITE_REQ_TOTAL = 4096
WRITE_HDR       = 8
WRITE_CHUNK_MAX = 4088                 # CONFIRMED cap; we use 4088 (<=4088)

# END_WRITE handler (0xb0970d6c) does copy_from_user(kbuf, arg, 28):
#   struct end_req { u32 len; u32 crc32; u32 idb_blk[5]; }   (28 bytes)
#     len       <= 0x3e800 (256000)               (0xb0970db4)  CONFIRMED
#     crc32     == rkcrc32(loader_buf, len)       (0xb0970dd4)  CONFIRMED (MUST match)
#     idb_blk[] = passed as write_idblock() arg3 (kbuf+8); the writer multiplies
#                 each entry by pages/block to place the 5 idblock copies.
# INFERRED default {2,3,4,5,6}: this is exactly what write_loader_lba fills in
# for a non-"type-4" NAND (0xb09709c4..0xb09709e8). For the other flash type it
# uses {0,2,4,6,8}. Override with --idb-blocks if your flash differs.
END_REQ_LEN     = 28
IDB_BLK_DEFAULT = [2, 3, 4, 5, 6]      # INFERRED

# ---- Rockchip CRC32 (poly 0x04C10DB7, MSB-first). Table verified byte-for-byte
#      against vmlinux @0xb1228500: table[1..3]=04c10db7,09821b6e,0d4316d9.
def _rk_crc_table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C10DB7) if (c & 0x80000000) else (c << 1)
            c &= 0xFFFFFFFF
        t.append(c)
    return t
_RKTAB = _rk_crc_table()
def rk_crc32(data: bytes, crc: int = 0) -> int:
    for b in data:
        crc = (_RKTAB[(b ^ (crc >> 24)) & 0xFF] ^ ((crc << 8) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return crc

def open_dev():
    return open(DEV, "rb+", buffering=0)

def do_write_sector(fd, dst_off: int, chunk: bytes):
    assert len(chunk) <= WRITE_CHUNK_MAX
    buf = bytearray(WRITE_REQ_TOTAL)
    struct.pack_into("<II", buf, 0, dst_off, len(chunk))
    buf[WRITE_HDR:WRITE_HDR + len(chunk)] = chunk
    cbuf = ctypes.create_string_buffer(bytes(buf), WRITE_REQ_TOTAL)
    fcntl.ioctl(fd, WRITE_SECTOR, cbuf, True)

def do_end_write(fd, total_len: int, crc: int, idb_blk):
    buf = bytearray(END_REQ_LEN)
    struct.pack_into("<II", buf, 0, total_len, crc)
    for i, v in enumerate(idb_blk[:5]):
        struct.pack_into("<I", buf, 8 + 4 * i, v)
    cbuf = ctypes.create_string_buffer(bytes(buf), END_REQ_LEN)
    fcntl.ioctl(fd, END_WRITE, cbuf, True)

def do_read_sector(fd, lba: int, nsec: int) -> bytes:
    assert 1 <= nsec <= 8                       # CONFIRMED cap (0xb0970c68)
    buf = ctypes.create_string_buffer(max(4096, nsec * SECTOR))
    struct.pack_into("<II", buf, 0, lba, nsec)  # {u32 lba; u32 nsec;}
    fcntl.ioctl(fd, READ_SECTOR, buf, True)
    return bytes(buf.raw[:nsec * SECTOR])

def do_get_flash_info(fd) -> bytes:
    buf = ctypes.create_string_buffer(64)       # handler copies 64 bytes back
    fcntl.ioctl(fd, GET_FLASH_INFO, buf, True)
    return bytes(buf.raw)

def write_loader(path, idb_blk, dry=False):
    img = open(path, "rb").read()
    if len(img) < 4:
        sys.exit("image too small")
    magic = struct.unpack_from("<I", img, 0)[0]
    if magic != IDB_MAGIC:
        sys.exit(
            "REFUSING: first word = 0x%08X, expected IDB magic 0x%08X.\n"
            "This is not an IDBlock. rk322x_loader_*.bin ('BOOT'=0x54004F42... "
            "actually 0x424F4F54) must be wrapped into an IDBlock first." % (magic, IDB_MAGIC))
    total = len(img)
    if total > 256000:
        sys.exit("IDBlock too large (%d > 256000)" % total)
    crc = rk_crc32(img, 0)                       # CONFIRMED: rkcrc32 over whole image
    print("image=%s  bytes=%d  sectors=%d  rkcrc32=0x%08X  idb_blocks=%s"
          % (path, total, (total + 511) // 512, crc, idb_blk))
    if dry:
        print("[dry-run] not touching device"); return
    with open_dev() as fd:
        off = 0
        while off < total:
            chunk = img[off:off + WRITE_CHUNK_MAX]
            do_write_sector(fd, off, chunk)      # accumulate into kernel RAM buf
            off += len(chunk)
        print("all chunks accumulated; committing via END_WRITE (-> write_idblock)")
        do_end_write(fd, total, crc, idb_blk)
        print("END_WRITE returned OK")

def main():
    ap = argparse.ArgumentParser(description="Write RK322x IDBlock via /dev/rknand_sys_storage")
    ap.add_argument("image", nargs="?", help="IDBlock image (first word must be 0xFCDC8C3B)")
    ap.add_argument("--idb-blocks", default=",".join(map(str, IDB_BLK_DEFAULT)),
                    help="comma list of 5 physical block indices for idb copies (INFERRED default 2,3,4,5,6)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--info", action="store_true", help="GET_FLASH_INFO + a READ_SECTOR probe, then exit")
    a = ap.parse_args()
    if a.info:
        with open_dev() as fd:
            fi = do_get_flash_info(fd)
            print("GET_FLASH_INFO(64B):", fi.hex())
            try:
                print("READ LBA0:", do_read_sector(fd, 0, 1)[:32].hex(), "...")
            except OSError as e:
                print("read lba0 failed:", e)
        return
    if not a.image:
        ap.error("image required (or use --info)")
    idb = [int(x) for x in a.idb_blocks.split(",")]
    write_loader(a.image, idb, dry=a.dry_run)

if __name__ == "__main__":
    main()

# ============================== BIG CAVEATS =================================
# (1) INPUT MUST BE AN IDBLOCK, NOT THE 'BOOT' LOADER.
#     write_idblock @0xb0970308 does: if (*(u32*)loader_buf != 0xFCDC8C3B) fail.
#     rk322x_loader_v1.10.256.bin starts with "BOOT" (0x424F4F54) -> rejected.
#     You must first build the IDBlock (sector0 tag 0xFCDC8C3B + FlashData +
#     FlashBoot) from the BOOT container. On RK322x that IDB is normally created
#     by the bootrom/usbplug during a `rkdeveloptool db <loader> ; wl ...` USB
#     session, or by boot_merger/mkkrnlimg's idb path. If you have a working
#     unit, dumping its first ~500 sectors gives you a ready IDBlock to re-flash.
#
# (2) THIS DOES NOT FIX rk_ftl_get_capacity()==0.
#     Capacity (RAM+0xf44, returned by FtlGetCap) is written ONLY by
#     FtlLoadSysInfo (existing FTL sys blocks) or FtlLowFormat (FtlLowFormat.c:112:
#     *(u32*)(iVar5+0xf44) = data_blocks * pages_per_block). write_idblock never
#     touches it. None of the sys_storage ioctls call FtlLowFormat -- it is only
#     reachable through the FTL vtable (vmlinux 0xb0dc6a18). So writing the loader
#     alone will NOT create /dev/rknand0. The user's hypothesis is incorrect.
#
#     GOOD NEWS: the ported blob EXPORTS the symbol `FtlLowFormat`
#     (drivers/nand-rk322x/rknand-port/rk_ftlv5_arm32.S:21432, .global FtlLowFormat).
#     The real bootstrap for a blank NAND is a one-line kernel call, e.g. in
#     rk_nand_blk.c nand_blk_register(), when rk_ftl_get_capacity()==0:
#           extern int FtlLowFormat(void);
#           if (rk_ftl_get_capacity() == 0) { FtlLowFormat(); }
#     After that, capacity is set, /dev/rknand0 appears, and you can then simply
#     write the IDBlock image to /dev/rknand0 at byte offset 64*512 -- the FtlWrite
#     hook (wrapper @0xb0970a78 -> write_loader_lba @0xb09708b8) auto-captures
#     sectors [64,564) and auto-commits the idb when LBA reaches 564. That avoids
#     the fragile END_WRITE block-index math entirely.
#
# (3) idb_blk[] (END_WRITE offset +8) is the physical block layout for the 5 idb
#     copies. Default {2,3,4,5,6} is what write_loader_lba uses for this flash;
#     verify against your part before trusting a commit.
# ===========================================================================
