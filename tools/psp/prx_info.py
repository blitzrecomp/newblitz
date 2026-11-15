#!/usr/bin/env python3
"""Module info, exports and imports of a PSP module's ELF (the .elf psp-prxgen turns into the .prx), standard library only:
the checks prxtool would give, for NB Remote's VSH plugin (docs/PSP_PORT.md "NB Remote").
  python3 -I tools/psp/prx_info.py build/psp/nbremote_plugin/launch/nbremote.elf [--forbid SysclibForKernel,...]
Prints the module name and attribute (0x0000 user, 0x1000 kernel), each export library and each import library with its
attribute and function count; --forbid exits 1 if any named library is imported."""
import struct
import sys


def main():
    path = sys.argv[1]
    forbid = []
    if "--forbid" in sys.argv:
        forbid = sys.argv[sys.argv.index("--forbid") + 1].split(",")
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise SystemExit("not an ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size = struct.unpack_from("<IIIIII", data, shoff + i * shentsize)
        secs.append([name, typ, addr, off, size])
    stro = secs[shstrndx][3]
    for s in secs:
        end = data.index(b"\0", stro + s[0])
        s[0] = data[stro + s[0]:end].decode()

    def read(addr, n):
        for name, typ, a, off, size in secs:
            if typ != 8 and a <= addr < a + size and a + size >= addr + n and size:
                return data[off + addr - a: off + addr - a + n]
        return None

    def cstr(addr):
        out = b""
        while True:
            c = read(addr, 1)
            if not c or c == b"\0":
                return out.decode("latin-1")
            out += c
            addr += 1

    sec = {s[0]: s for s in secs}
    mi = sec.get(".rodata.sceModuleInfo")
    attr, v1, v0 = struct.unpack_from("<HBB", data, mi[3])
    name = data[mi[3] + 4: mi[3] + 32].split(b"\0")[0].decode()
    gp, ent_top, ent_end, stub_top, stub_end = struct.unpack_from("<IIIII", data, mi[3] + 32)
    print(f"module {name!r} attr 0x{attr:04X} ({'kernel' if attr & 0x1000 else 'user'}) version {v0}.{v1}")
    a = ent_top
    while a < ent_end:
        lname, ver, lattr, elen, vc, fc, ents = struct.unpack("<IHHBBHI", read(a, 16))
        print(f"  export {cstr(lname) if lname else '(module)':<22} attr 0x{lattr:04X} {fc} functions {vc} variables")
        a += elen * 4
    bad = []
    a = stub_top
    while a < stub_end:
        lname, ver, lattr, elen, vc, fc, nids, funcs = struct.unpack("<IHHBBHII", read(a, 20))
        ln = cstr(lname)
        print(f"  import {ln:<22} attr 0x{lattr:04X} {fc} functions")
        if ln in forbid:
            bad.append(ln)
        a += elen * 4
    if bad:
        print("FORBIDDEN imports: " + ", ".join(bad))
        sys.exit(1)


if __name__ == "__main__":
    main()
