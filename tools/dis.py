#!/usr/bin/env python3
"""Disassemble a function or range straight from the ROM, and list its callers.

Usage:
  dis.py ADDR [COUNT]           disassemble COUNT words (default: up to the function's end)
  dis.py --callers ADDR         every jal to ADDR across all segments
  dis.py --section ovl_b ADDR   pick the overlay when ADDR is in shared overlay VRAM
"""
import argparse
import sys
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_syms import ROM, SEGMENTS  # noqa: E402


def segment_for(addr: int, name: str | None):
    for seg in SEGMENTS:
        if (name is None or seg[0] == name) and seg[2] <= addr < seg[3]:
            return seg
    raise SystemExit(f"0x{addr:08X} is not in any CPU text segment" + (f" named {name}" if name else ""))


def words(rom: bytes, seg, start: int, count: int):
    _, rom_start, vram, _ = seg
    for i in range(count):
        a = start + 4 * i
        o = rom_start + (a - vram)
        yield a, int.from_bytes(rom[o:o + 4], "big")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", type=lambda x: int(x, 16))
    ap.add_argument("count", type=int, nargs="?")
    ap.add_argument("--callers", action="store_true")
    ap.add_argument("--section")
    a = ap.parse_args()
    rom = ROM.read_bytes()

    if a.callers:
        target = (a.addr >> 2) & 0x03FFFFFF
        for seg in SEGMENTS:
            for addr, w in words(rom, seg, seg[2], (seg[3] - seg[2]) // 4):
                if w >> 26 == 3 and w & 0x03FFFFFF == target:
                    print(f"{seg[0]:6} 0x{addr:08X}")
        return 0

    seg = segment_for(a.addr, a.section)
    n = a.count or (seg[3] - a.addr) // 4
    jr_ra_seen = None
    for addr, w in words(rom, seg, a.addr, n):
        print(f"{addr:08X}  {w:08X}  {rabbitizer.Instruction(w, vram=addr).disassemble()}")
        if a.count is None:
            if w == 0x03E00008:
                jr_ra_seen = addr
            elif jr_ra_seen is not None and addr == jr_ra_seen + 4:
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())
