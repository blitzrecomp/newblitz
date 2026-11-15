#!/usr/bin/env python3
"""Name libultra/libc functions by matching n64sym's signature database.

A signature is (name, size, crcA, crcB, relocs): crcA is the CRC32 of the first
8 bytes and crcB of the whole body, both taken after relocated fields are
masked out (low 16 bits for .hi16/.lo16, low 26 bits for .targ26). Same rules
as n64sym's CSignatureFile::TestSymbol. Database: ext/n64sym (MIT).

Usage: sigmatch.py ROM ROM_START VRAM SIZE   (prints "vram size name")
"""
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIG_PATH = ROOT / "ext" / "n64sym" / "src" / "builtin_signatures.sig"

MASKS = {".hi16": 0xFFFF0000, ".lo16": 0xFFFF0000, ".targ26": 0xFC000000}


class Signature:
    __slots__ = ("name", "size", "crc_a", "crc_b", "relocs")

    def __init__(self, name, size, crc_a, crc_b):
        self.name, self.size, self.crc_a, self.crc_b = name, size, crc_a, crc_b
        self.relocs = {}  # offset -> (type, target name)


def load_signatures(path: Path = SIG_PATH) -> list[Signature]:
    sigs = []
    for line in path.read_text().splitlines():
        parts = line.split()
        if not parts or parts[0].startswith("#"):
            continue
        if parts[0].startswith("."):
            for off in parts[2:]:
                sigs[-1].relocs[int(off, 16)] = (parts[0], parts[1])
        else:
            sigs.append(Signature(parts[0], int(parts[1], 16), int(parts[2], 16), int(parts[3], 16)))
    return sigs


def masked(body: bytes, sig: Signature) -> bytes:
    if not sig.relocs:
        return body
    b = bytearray(body)
    for off, (kind, _) in sig.relocs.items():
        if off + 4 <= len(b):
            w = int.from_bytes(b[off:off + 4], "big") & MASKS[kind]
            b[off:off + 4] = w.to_bytes(4, "big")
    return bytes(b)


def test(sig: Signature, data: bytes, off: int) -> bool:
    body = data[off:off + sig.size]
    if len(body) < sig.size:
        return False
    head = masked(body[:8], sig) if sig.relocs and min(sig.relocs) < 8 else body[:8]
    if zlib.crc32(head[:min(sig.size, 8)]) != sig.crc_a:
        return False
    return zlib.crc32(masked(body, sig)) == sig.crc_b


def candidate_offsets(data: bytes) -> list[int]:
    """Likely function starts, as n64sym guesses them: after `jr ra; <delay>`, and at `addiu sp, sp, -n`."""
    out = {0}
    for i in range(0, len(data) - 12, 4):
        w = int.from_bytes(data[i:i + 4], "big")
        if w == 0x03E00008 and data[i + 8:i + 12] != b"\0\0\0\0":
            out.add(i + 8)
        if (w & 0xFFFF0000) == 0x27BD0000 and (w & 0x8000):
            out.add(i)
    # skip padding nops after jr ra
    for i in list(out):
        j = i
        while j < len(data) - 4 and data[j:j + 4] == b"\0\0\0\0":
            j += 4
        out.add(j)
    return sorted(out)


class SigIndex:
    """Signatures indexed for lookup at a candidate function start."""

    def __init__(self, sigs: list[Signature]):
        self.plain_head = {}  # crcA -> sigs with no relocation in the first 8 bytes
        self.reloc_head = []  # sigs whose first 8 bytes need masking: tested one by one
        for s in sigs:
            if s.relocs and min(s.relocs) < 8:
                self.reloc_head.append(s)
            else:
                self.plain_head.setdefault(s.crc_a, []).append(s)

    def candidates(self, data: bytes, off: int):
        yield from self.plain_head.get(zlib.crc32(data[off:off + 8]), ())
        yield from self.reloc_head

    def best_match(self, data: bytes, off: int, max_size: int | None = None):
        """Longest signature matching at off, at most max_size bytes long."""
        best = None
        for s in self.candidates(data, off):
            if (max_size is None or s.size <= max_size) and (best is None or s.size > best.size) and test(s, data, off):
                best = s
        return best


def match(data: bytes, vram: int, index: SigIndex, offsets=None) -> list[tuple[int, int, str, Signature]]:
    """Return (vram, size, name, sig) for each matched function."""
    results = []
    for off in offsets if offsets is not None else candidate_offsets(data):
        if best := index.best_match(data, off):
            results.append((vram + off, best.size, best.name, best))
    return results


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 1
    rom = Path(sys.argv[1]).read_bytes()
    start, vram, size = (int(x, 0) for x in sys.argv[2:])
    index = SigIndex(load_signatures())
    for addr, sz, name, _ in match(rom[start:start + size], vram, index):
        print(f"{addr:08X} {sz:5X} {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
