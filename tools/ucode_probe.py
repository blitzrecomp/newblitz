#!/usr/bin/env python3
"""Check which RT64-known graphics microcodes are present in the ROM.

RT64 picks its HLE implementation by hashing the first N bytes of the ucode
text and data segments (XXH3-64) and looking them up in the table in
src/gbi/rt64_gbi.cpp. This scans the ROM for every aligned window that
matches an entry in that table, so we know before M2 whether RT64 will
recognise the game's microcode.

RT64 hashes RDRAM as N64ModernRuntime stores it: each 32-bit word
byte-reversed. The ROM is word-swapped the same way before hashing.

Usage: ucode_probe.py RT64_GBI_CPP [ROM]   (ROM defaults to build/baserom.z64)
Needs: xxhash (in .venv)
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

import xxhash

ROOT = Path(__file__).resolve().parent.parent
ALIGN = 8

SEGMENT = re.compile(r"GBISegment\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)ULL,\s*\{([^}]*)\}\s*\}")
INSTANCE = re.compile(r'const GBIInstance\s+(\w+)\s*=\s*\{\s*"([^"]+)",\s*GBIUCode::(\w+)')


def parse_table(src: str):
    instances = {sym: (name, kind) for sym, name, kind in INSTANCE.findall(src)}
    # Text segments are listed before data segments in RT64's database.
    text_start = src.find("textSegments")
    data_start = src.find("dataSegments", text_start + 1)
    segments = []
    for m in SEGMENT.finditer(src):
        length, value = int(m.group(1), 16), int(m.group(2), 16)
        syms = [s.strip().lstrip("&") for s in m.group(3).split(",") if s.strip()]
        kind = "data" if data_start != -1 and m.start() > data_start else "text"
        segments.append((kind, length, value, syms))
    return instances, segments


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    instances, segments = parse_table(Path(sys.argv[1]).read_text())
    z64 = Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "build" / "baserom.z64").read_bytes()
    rom = bytearray(z64)
    rom[0::4], rom[1::4], rom[2::4], rom[3::4] = z64[3::4], z64[2::4], z64[1::4], z64[0::4]
    rom = bytes(rom)

    by_length = defaultdict(dict)
    for kind, length, value, syms in segments:
        by_length[length][value] = (kind, syms)

    hits = []
    for length, table in sorted(by_length.items()):
        for off in range(0, len(rom) - length, ALIGN):
            h = xxhash.xxh3_64_intdigest(rom[off:off + length])
            if h in table:
                kind, syms = table[h]
                hits.append((off, kind, length, syms))

    if not hits:
        print("No RT64-known microcode segment found (ucode may be compressed in ROM; check RAM at runtime).")
        return 2
    for off, kind, length, syms in sorted(hits):
        names = ", ".join(f"{instances.get(s, (s, '?'))[0]} [{instances.get(s, ('', '?'))[1]}]" for s in syms)
        print(f"ROM 0x{off:07X}  {kind:4}  len 0x{length:04X}  -> {names}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
