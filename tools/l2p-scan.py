#!/usr/bin/env python3
# Phase-A rknand L2P reconstruction — pass 1: build the logical->physical map.
# Input: `nanddump -o -f /dev/stdout /dev/mtd0` binary stream = [8192 data | 744 oob]/page.
# Decodes per-page FTL metadata (de-rotating mainline's sys-byte order), keeps the newest
# version per LPN, saves the map, and reports stats incl. whether LPN 0/1 (fs start) exist.
#   nanddump -o -f /dev/stdout /dev/mtd0 | l2p-scan.py /root/l2p.map
import sys, struct, array
PAGE, OOB, STEPS = 8192, 744, 8
CHUNK = PAGE + OOB
DATA_MAGIC = 0xF095

def sys_word(oob, step):
    pos = (STEPS - 1) if step == 0 else (step - 1)
    return struct.unpack_from('<I', oob, pos * 4)[0]

def newer(a, b):                     # wrap-around: a newer than b
    return ((a - b) & 0xFFFFFFFF) < 0x80000001

mapfile = sys.argv[1] if len(sys.argv) > 1 else None
f = sys.stdin.buffer
lpn2phys = {}       # lpn -> (physpage, version)
counts = {}; page = 0; datap = 0
while True:
    c = f.read(CHUNK)
    if len(c) < CHUNK: break
    oob = c[PAGE:PAGE+32]
    s0 = sys_word(oob, 0); magic = s0 & 0xFFFF
    counts[magic] = counts.get(magic, 0) + 1
    if magic == DATA_MAGIC:
        datap += 1
        ver = sys_word(oob, 1); lpn = sys_word(oob, 2)
        cur = lpn2phys.get(lpn)
        if cur is None or newer(ver, cur[1]):
            lpn2phys[lpn] = (page, ver)
    page += 1
    if page % 100000 == 0:
        sys.stderr.write(f"  ...{page} pages, {len(lpn2phys)} LPNs\n")

print(f"scanned {page} physical pages; data pages {datap}; distinct LPNs {len(lpn2phys)}")
print("magics:", {hex(k): v for k, v in sorted(counts.items(), key=lambda x: -x[1])[:8]})
if lpn2phys:
    lo, hi = min(lpn2phys), max(lpn2phys)
    print(f"LPN range {lo}..{hi}; logical image size ~{(hi+2)*4096/1e9:.2f} GB")
    for l in (0, 1, 2, 3):
        p = lpn2phys.get(l)
        print(f"  LPN {l}: {'physpage '+str(p[0]) if p else 'MISSING'}")
if mapfile:
    hi = max(lpn2phys)
    arr = array.array('i', [-1]) * (hi + 2)
    for l, (p, v) in lpn2phys.items():
        arr[l] = p
    with open(mapfile, 'wb') as o:
        o.write(arr.tobytes())
    print(f"saved map ({len(arr)} entries) -> {mapfile}")
