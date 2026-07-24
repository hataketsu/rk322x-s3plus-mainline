#!/usr/bin/env python3
# Phase-A rknand L2P reconstruction via MEMREADOOB ioctl (real per-page seeking; the
# rockchip NFC driver descrambles OOB on read, so the FTL metadata is recovered directly).
# Pass 1: scan every physical page's OOB, decode {magic,version,LPN,prevPPA} (de-rotating
# mainline's sys-byte order), keep newest version per LPN, save the logical->physical map.
#   l2p-scan.py /dev/mtd0 /root/l2p.map
import sys, os, fcntl, ctypes, struct, array
PAGE, STEPS = 8192, 8
MEMREADOOB = 0xC00C4D04                      # _IOWR('M',4, mtd_oob_buf) on arm32 (12 bytes)
MEMGETINFO = 0x80204d01                      # _IOR('M',1, mtd_info_user) -> get size
DATA_MAGIC = 0xF095
dev = sys.argv[1]; mapfile = sys.argv[2] if len(sys.argv) > 2 else None

class ob(ctypes.Structure):
    _fields_ = [("start", ctypes.c_uint32), ("length", ctypes.c_uint32), ("ptr", ctypes.c_void_p)]

fd = os.open(dev, os.O_RDONLY)
info = bytearray(32)
fcntl.ioctl(fd, MEMGETINFO, info, True)
size = struct.unpack_from("<I", info, 8)[0]
    if size < (1<<20): size = 0x200000000
npages = size // PAGE
sys.stderr.write(f"device {size} bytes, {npages} pages\n")

buf = ctypes.create_string_buffer(64)
req = ob(0, 64, ctypes.cast(buf, ctypes.c_void_p))
def sw(o, s):
    p = (STEPS - 1) if s == 0 else (s - 1)
    return struct.unpack_from('<I', o, p * 4)[0]
def newer(a, b): return ((a - b) & 0xFFFFFFFF) < 0x80000001

lpn2phys = {}; counts = {}; datap = 0; err = 0
for pg in range(npages):
    req.start = pg * PAGE
    try:
        fcntl.ioctl(fd, MEMREADOOB, req)
    except OSError:
        err += 1; continue
    o = buf.raw[:32]
    magic = sw(o, 0) & 0xFFFF
    counts[magic] = counts.get(magic, 0) + 1
    if magic == DATA_MAGIC:
        datap += 1
        ver = sw(o, 1); lpn = sw(o, 2)
        cur = lpn2phys.get(lpn)
        if cur is None or newer(ver, cur[1]):
            lpn2phys[lpn] = (pg, ver)
    if pg % 100000 == 0:
        sys.stderr.write(f"  {pg}/{npages} pages, {len(lpn2phys)} LPNs, {datap} data, {err} err\n")

print(f"scanned {npages} pages; data {datap}; distinct LPNs {len(lpn2phys)}; ioctl_err {err}")
print("magics:", {hex(k): v for k, v in sorted(counts.items(), key=lambda x: -x[1])[:6]})
if lpn2phys:
    lo, hi = min(lpn2phys), max(lpn2phys)
    print(f"LPN {lo}..{hi}; logical image ~{(hi+1)*PAGE/1e9:.2f} GB")
    for l in range(6):
        p = lpn2phys.get(l); print(f"  LPN {l}: {'physpage '+str(p[0]) if p else 'MISSING'}")
    if mapfile:
        arr = array.array('i', [-1]) * (hi + 1)
        for l, (p, v) in lpn2phys.items(): arr[l] = p
        open(mapfile, 'wb').write(arr.tobytes())
        print(f"saved map ({hi+1} entries) -> {mapfile}")
