"""Offline helper for the guest image dumped by XENIA_CRASH_DUMP_IMAGE.

usage: .venv/bin/python ppc.py dis 82AE01A8 82AE0300
       .venv/bin/python ppc.py xref 83179D94        # bl call sites to an address
       .venv/bin/python ppc.py func 82AE9EC8        # disassemble the containing function
       .venv/bin/python ppc.py str 4                # find ASCII strings >= n chars
"""
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

BASE = 0x82000000
IMG = open(__file__.rsplit("/", 1)[0] + "/ks_image.bin", "rb").read()
md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)


def word(addr):
    return struct.unpack(">I", IMG[addr - BASE : addr - BASE + 4])[0]


def dis(start, end):
    for addr in range(start, end, 4):
        code = IMG[addr - BASE : addr - BASE + 4]
        ins = next(md.disasm(code, addr), None)
        text = f"{ins.mnemonic:8} {ins.op_str}" if ins else "??"
        print(f"{addr:08X}  {code.hex().upper()}  {text}")


def drefs(target):
    """Data references: pointer-table words and lis/addi(ori) pairs loading target."""
    out = []
    hi = (target >> 16) & 0xFFFF
    lo = target & 0xFFFF
    adj = hi + 1 if lo & 0x8000 else hi  # addi sign-extends the low half
    for addr in range(BASE, BASE + len(IMG) - 4, 4):
        w = word(addr)
        if w == target:
            out.append((addr, "word"))
        if (w >> 26) == 15 and (w & 0xFFFF) in (hi, adj):  # lis rD, imm
            rd = (w >> 21) & 31
            for k in range(1, 9):
                if addr + 4 * k + 4 > BASE + len(IMG):
                    break
                n = word(addr + 4 * k)
                op = n >> 26
                if op in (14, 24) and ((n >> 16) & 31) == rd and (n & 0xFFFF) == lo:
                    out.append((addr + 4 * k, "lis+%s" % ("addi" if op == 14 else "ori")))
                    break
    return out


def xrefs(target):
    out = []
    for addr in range(BASE, BASE + len(IMG) - 4, 4):
        w = word(addr)
        if (w >> 26) == 18 and (w & 3) == 1:  # bl
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            if (addr + li) & 0xFFFFFFFF == target:
                out.append(addr)
    return out


def func_start(addr):
    # prologue: `mfspr r12,lr` (0x7D8802A6) often followed by a save helper `bl`.
    a = addr & ~3
    while a > BASE:
        if word(a) == 0x7D8802A6:
            return a
        a -= 4
    return None


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "dis":
        dis(int(sys.argv[2], 16), int(sys.argv[3], 16))
    elif cmd == "dref":
        for a, kind in drefs(int(sys.argv[2], 16)):
            print(f"{a:08X}  {kind}")
    elif cmd == "xref":
        for a in xrefs(int(sys.argv[2], 16)):
            print(f"{a:08X}  (func {func_start(a):08X})")
    elif cmd == "func":
        a = int(sys.argv[2], 16)
        s = func_start(a)
        e = s + 4
        while e < BASE + len(IMG) and word(e) != 0x7D8802A6 and e - s < 0x2000:
            e += 4
        dis(s, e)
    elif cmd == "str":
        import re

        n = int(sys.argv[2]) if len(sys.argv) > 2 else 6
        for m in re.finditer(rb"[\x20-\x7e]{%d,}" % n, IMG):
            print(f"{BASE + m.start():08X}  {m.group().decode()}")
