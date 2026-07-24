#!/usr/bin/env python3
# Phase-A pass 2: reconstruct the logical image from the L2P map built by l2p-scan.py.
# For each logical page LPN in [start,start+count), read its mapped physical page's data
# (os.pread on the descrambling mtd char device) and place it at logical offset LPN*8192.
# Unmapped LPNs (holes) are zero-filled. Emit to a file to loop-mount / inspect.
#   l2p-extract.py /root/l2p.map /dev/mtd0 /root/out.img [start_lpn] [count_lpn]
import sys, os, array
PAGE = 8192
mapfile, dev, out = sys.argv[1], sys.argv[2], sys.argv[3]
start = int(sys.argv[4]) if len(sys.argv) > 4 else 0
count = int(sys.argv[5]) if len(sys.argv) > 5 else 0
m = array.array('i'); m.frombytes(open(mapfile, 'rb').read())
if count == 0: count = len(m) - start
fd = os.open(dev, os.O_RDONLY)
zero = b'\x00' * PAGE
holes = 0
with open(out, 'wb') as o:
    for lpn in range(start, start + count):
        phys = m[lpn] if lpn < len(m) else -1
        if phys < 0:
            o.write(zero); holes += 1
        else:
            d = os.pread(fd, PAGE, phys * PAGE)
            o.write(d if len(d) == PAGE else (d + zero)[:PAGE])
os.close(fd)
sys.stderr.write(f"wrote {count} logical pages ({count*PAGE} bytes) from LPN {start}; holes {holes}\n")
