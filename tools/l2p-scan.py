#!/usr/bin/env python3
# Phase-A rknand L2P reconstruction (window-driven). Reads /dev/mtd0 in windows via
# nanddump subprocess (python controls offsets so physical-page indices stay exact and
# error-prone whole-device reads are avoided), decodes per-page FTL OOB metadata
# (de-rotating mainline's sys-byte order), keeps newest version per LPN, saves the map.
#   l2p-scan.py /dev/mtd0 /root/l2p.map [total_mb] [win_mb]
import sys, struct, array, subprocess
PAGE, OOB, STEPS = 8192, 744, 8
CHUNK = PAGE + OOB
DATA_MAGIC = 0xF095
dev = sys.argv[1]
mapfile = sys.argv[2] if len(sys.argv) > 2 else None
TOTAL_MB = int(sys.argv[3]) if len(sys.argv) > 3 else 8192
WIN_MB = int(sys.argv[4]) if len(sys.argv) > 4 else 16

def sys_word(oob, step):
    pos = (STEPS - 1) if step == 0 else (step - 1)
    return struct.unpack_from('<I', oob, pos * 4)[0]
def newer(a, b):
    return ((a - b) & 0xFFFFFFFF) < 0x80000001

lpn2phys = {}; counts = {}; datap = 0; total_pages = 0; readfail = 0
for off_mb in range(0, TOTAL_MB, WIN_MB):
    base_page = off_mb * 1024 * 1024 // PAGE
    try:
        r = subprocess.run(['nanddump','-o','-f','/dev/stdout','-l',str(WIN_MB*1024*1024),
                            '-s',str(off_mb*1024*1024), dev],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=120)
        buf = r.stdout
    except Exception:
        readfail += 1; continue
    npages = len(buf) // CHUNK
    for i in range(npages):
        oob = buf[i*CHUNK+PAGE : i*CHUNK+PAGE+32]
        s0 = sys_word(oob, 0); magic = s0 & 0xFFFF
        counts[magic] = counts.get(magic, 0) + 1
        if magic == DATA_MAGIC:
            datap += 1
            ver = sys_word(oob, 1); lpn = sys_word(oob, 2)
            cur = lpn2phys.get(lpn)
            if cur is None or newer(ver, cur[1]):
                lpn2phys[lpn] = (base_page + i, ver)
    total_pages += npages
    if off_mb % 512 == 0:
        sys.stderr.write(f"  {off_mb}MB: {total_pages} pages, {len(lpn2phys)} LPNs, {datap} data\n")

print(f"scanned {total_pages} pages; data {datap}; distinct LPNs {len(lpn2phys)}; readfail_windows {readfail}")
print("magics:", {hex(k): v for k, v in sorted(counts.items(), key=lambda x: -x[1])[:6]})
if lpn2phys:
    lo, hi = min(lpn2phys), max(lpn2phys)
    print(f"LPN {lo}..{hi}; image ~{(hi+1)*8192/1e9:.2f} GB")
    for l in (0,1,2,3,4,5):
        p = lpn2phys.get(l); print(f"  LPN {l}: {'physpage '+str(p[0]) if p else 'MISSING'}")
    if mapfile:
        arr = array.array('i', [-1]) * (hi + 1)
        for l,(p,v) in lpn2phys.items(): arr[l] = p
        open(mapfile,'wb').write(arr.tobytes())
        print(f"saved map ({hi+1} entries) -> {mapfile}")
