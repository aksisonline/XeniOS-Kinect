"""Minimal XDVDFS (Xbox 360 disc) reader: list and extract files from an ISO."""
import struct, sys
SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"

def open_iso(path):
    f = open(path, "rb")
    for base in (0x0, 0xFD90000, 0x2080000, 0x18300000):
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            f.seek(base + 32 * SECTOR + 20)
            root_sector, root_size = struct.unpack("<II", f.read(8))
            return f, base, root_sector, root_size
    raise SystemExit("not an XDVDFS image")

def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    out = []
    def visit(off):
        if off * 4 >= len(data): return
        o = off * 4
        left, right, sec, sz, attr, nlen = struct.unpack("<HHIIBB", data[o:o + 14])
        if left == 0xFFFF and right == 0xFFFF and sec == 0xFFFFFFFF: return
        name = data[o + 14:o + 14 + nlen].decode("latin1")
        if left: visit(left)
        if attr & 0x10:
            out.extend(walk(f, base, sec, sz, prefix + name + "\\"))
        else:
            out.append((prefix + name, sec, sz))
        if right: visit(right)
    visit(0)
    return out

if __name__ == "__main__":
    iso = sys.argv[1]
    f, base, rs, rz = open_iso(iso)
    files = walk(f, base, rs, rz)
    pat = sys.argv[2].lower() if len(sys.argv) > 2 else ""
    for name, sec, sz in files:
        if pat in name.lower():
            f.seek(base + sec * SECTOR)
            print(f"{sz:>10}  {name}   magic={f.read(4)!r}")
    if len(sys.argv) > 3:   # extract matching files
        import os
        for name, sec, sz in files:
            if pat in name.lower():
                f.seek(base + sec * SECTOR)
                os.makedirs("extract", exist_ok=True)
                open("extract/" + name.replace("\\", "_"), "wb").write(f.read(sz))
