#!/usr/bin/env python3
"""bmp2png.py in.bmp out.png — converts 24-bit or 32-bit bottom-up BMPs to PNG."""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack_from('<ii', d, 18)
bpp = struct.unpack_from('<H', d, 28)[0]
step = bpp // 8
row = w * step
raw = bytearray()
for y in range(h - 1, -1, -1):
    line = d[54 + y * row: 54 + (y + 1) * row]
    rgb = bytearray(w * 3)
    rgb[0::3] = line[2::step]
    rgb[1::3] = line[1::step]
    rgb[2::3] = line[0::step]
    raw += b'\0' + rgb
def chunk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
