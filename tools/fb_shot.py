#!/usr/bin/env python3
"""Saves the current frame of a running tools/play_wslg.sh session (/dev/shm/ew3fb) as a PNG.
Usage: python3 tools/fb_shot.py [out.png] [fb file]"""
import struct, sys, zlib
out = sys.argv[1] if len(sys.argv) > 1 else "scratch/fb.png"
src = sys.argv[2] if len(sys.argv) > 2 else "/dev/shm/ew3fb"
d = open(src, "rb").read()
magic, w, h, seq = struct.unpack_from("<4I", d)
px = d[16:16 + w * h * 4]
rows = [b"\0" + px[(h - 1 - y) * w * 4:(h - y) * w * 4] for y in range(h)]   # bottom-up -> top-down
def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
      chunk(b"IDAT", zlib.compress(b"".join(rows), 3)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
print("frame %d -> %s" % (seq, out))
