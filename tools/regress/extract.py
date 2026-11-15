#!/usr/bin/env python3
"""extract.py DUMP OUT ADDR:LEN ...: RDRAM ranges from a tools/live.sh dump (little-endian words) in game byte order."""
import sys

b = open(sys.argv[1], "rb").read()


def rd(a, n):
    o = a - 0x80000000
    out = bytearray()
    for i in range(o & ~3, o + n + 4, 4):
        out += b[i:i + 4][::-1]
    s = o - (o & ~3)
    return bytes(out[s:s + n])


out = b""
for r in sys.argv[3:]:
    a, n = r.split(":")
    out += rd(int(a, 16), int(n, 16))
open(sys.argv[2], "wb").write(out)
