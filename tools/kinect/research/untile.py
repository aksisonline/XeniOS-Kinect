#!/usr/bin/env python3
"""Detile a Xenos 2D-tiled 32bpp dump (resolve dumps from XENIA_RESOLVE_DUMP) to PNG.
usage: untile.py dump.bin width height out.png [channel_order]"""
import sys
from PIL import Image

def tiled_offset(x, y, width, log2_bpp):
    aligned = (width + 31) & ~31
    macro = ((y >> 5) * (aligned >> 5) + (x >> 5)) << (log2_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log2_bpp
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((micro & 32) << 3)
    return (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) +
            ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))

src, w, h, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
order = sys.argv[5] if len(sys.argv) > 5 else "0123"
d = open(src, "rb").read()
img = Image.new("RGB", (w, h))
px = img.load()
for y in range(h):
    for x in range(w):
        o = tiled_offset(x, y, w, 2)
        if o + 4 > len(d):
            continue
        b = d[o:o + 4]
        px[x, y] = (b[int(order[1])], b[int(order[2])], b[int(order[3])])
img.save(out)
