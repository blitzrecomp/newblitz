#!/usr/bin/env python3
"""The bundled v1.0 -> v1.1 ROM patch (assets/rom/v10_to_v11.nbdiff), and a reader for it.

NFL Blitz 2000 USA v1.0 (NBIE, md5 ba49514441023722f02d41c62612f6c3) is the same game as the v1.1 New Blitz plays, with
its code moved: the assets are byte for byte the same. The app rebuilds v1.1 from a v1.0 dump at import
(src/main/rom_upgrade.cpp), with this patch: bsdiff's edit list (control triples, a diff block added to the old bytes,
and literal "extra" bytes), re-packed so the app reads it with the zstd decoder it already links (RT64's) instead of
bzip2. The patch holds no assets, only about 42 KB of v1.1's own code bytes (as the diff and extra blocks).

  tools/make_rom_patch.py make --old v10.z64 --new v11.z64 [--bsdiff v10_to_v11.bsdiff] [--out assets/rom/v10_to_v11.nbdiff]
  tools/make_rom_patch.py apply <v1.0 .z64> <out .z64> [--patch assets/rom/v10_to_v11.nbdiff]

make: runs bsdiff (Homebrew's, `brew install bsdiff`) unless --bsdiff names one, and zstd for the two frames; it checks the
result by applying it before writing. apply: needs the zstd command (romtool.py uses it for a v1.0 dump).

The format, all little-endian:
  0   "NBDIFF01"
  8   u32 old size, u32 new size, u32 control count
  20  old's SHA-1 (20 bytes), new's SHA-1 (20 bytes)
  60  u32 control frame size, u32 data frame size
  68  zstd frame: control count x (u32 add, u32 copy, i32 seek)
      zstd frame: new size bytes, in the new file's order: for an "add" run, the difference to add to the old bytes at the
      old position; for a "copy" run, the new bytes themselves. So the app decompresses straight into the new ROM, then
      adds the old bytes over the add runs (no 16 MB diff buffer of its own).
Applying: new_pos = old_pos = 0; for each (add, copy, seek): new[new_pos + i] += old[old_pos + i] for i < add (old bytes
past the end count as 0); new_pos += add + copy; old_pos += add + seek.
"""
from __future__ import annotations

import argparse
import bz2
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PATCH = ROOT / "assets" / "rom" / "v10_to_v11.nbdiff"
MAGIC = b"NBDIFF01"
HEADER = struct.Struct("<8sIII20s20sII")


def _offtin(b: bytes) -> int:
    y = struct.unpack("<Q", b)[0]
    return -(y & ~(1 << 63)) if y >> 63 else y


def read_bsdiff(data: bytes) -> tuple[list[tuple[int, int, int]], bytes, bytes, int]:
    if data[:8] != b"BSDIFF40":
        raise SystemExit("not a BSDIFF40 patch")
    cl, dl, ns = _offtin(data[8:16]), _offtin(data[16:24]), _offtin(data[24:32])
    ctrl = bz2.decompress(data[32:32 + cl])
    diff = bz2.decompress(data[32 + cl:32 + cl + dl])
    extra = bz2.decompress(data[32 + cl + dl:])
    triples = [(_offtin(ctrl[i:i + 8]), _offtin(ctrl[i + 8:i + 16]), _offtin(ctrl[i + 16:i + 24])) for i in range(0, len(ctrl), 24)]
    return triples, diff, extra, ns


def zstd(args: list[str], data: bytes) -> bytes:
    exe = shutil.which("zstd")
    if exe is None:
        raise SystemExit("the zstd command is needed (brew install zstd)")
    return subprocess.run([exe, "-q", *args], input=data, stdout=subprocess.PIPE, check=True).stdout


def apply(old: bytes, patch: bytes) -> bytes:
    magic, old_size, new_size, count, old_sha, new_sha, csize, dsize = HEADER.unpack_from(patch)
    if magic != MAGIC:
        raise SystemExit("not an NBDIFF01 patch")
    if len(old) != old_size or hashlib.sha1(old).digest() != old_sha:
        raise SystemExit("that isn't the ROM this patch is for")
    ctrl = zstd(["-d", "-c"], patch[HEADER.size:HEADER.size + csize])
    new = bytearray(zstd(["-d", "-c"], patch[HEADER.size + csize:HEADER.size + csize + dsize]))
    if len(ctrl) != 12 * count or len(new) != new_size:
        raise SystemExit("the patch is damaged")
    npos = opos = 0
    for i in range(count):
        add, copy, seek = struct.unpack_from("<IIi", ctrl, 12 * i)
        if npos + add + copy > new_size:
            raise SystemExit("the patch is damaged")
        # The old bytes under this run (0 outside the old file), with the few non-zero differences added.
        lo, hi = max(opos, 0), min(opos + add, old_size)
        run = bytearray(max(lo - opos, 0)) + bytearray(old[lo:hi]) if hi > lo else bytearray()
        run += bytearray(add - len(run))
        diff = bytes(new[npos:npos + add])
        for m in re.finditer(rb"[^\x00]+", diff):
            for k in range(m.start(), m.end()):
                run[k] = (run[k] + diff[k]) & 0xFF
        new[npos:npos + add] = run
        npos += add + copy
        opos += add + seek
    if hashlib.sha1(new).digest() != new_sha:
        raise SystemExit("the patched ROM's SHA-1 is wrong")
    return bytes(new)


def cmd_make(a) -> int:
    old, new = Path(a.old).read_bytes(), Path(a.new).read_bytes()
    if a.bsdiff:
        bs = Path(a.bsdiff).read_bytes()
    else:
        exe = shutil.which("bsdiff")
        if exe is None:
            raise SystemExit("bsdiff is needed (brew install bsdiff), or --bsdiff <patch>")
        with tempfile.TemporaryDirectory() as t:
            subprocess.run([exe, a.old, a.new, f"{t}/p"], check=True)
            bs = Path(f"{t}/p").read_bytes()
    triples, diff, extra, ns = read_bsdiff(bs)
    if ns != len(new):
        raise SystemExit("the bsdiff patch makes a file of another size")
    ctrl = bytearray()
    data = bytearray()
    di = ei = 0
    for add, copy, seek in triples:
        ctrl += struct.pack("<IIi", add, copy, seek)
        data += diff[di:di + add]
        data += extra[ei:ei + copy]
        di += add
        ei += copy
    if len(data) != len(new):
        raise SystemExit("the bsdiff patch's runs don't cover the new file")
    cz = zstd(["-19", "-c", f"--stream-size={len(ctrl)}"], bytes(ctrl))  # the sizes go in the frames
    dz = zstd(["--ultra", "-22", "-c", f"--stream-size={len(data)}"], bytes(data))
    out = HEADER.pack(MAGIC, len(old), len(new), len(triples), hashlib.sha1(old).digest(), hashlib.sha1(new).digest(),
                      len(cz), len(dz)) + cz + dz
    if apply(old, out) != new:
        raise SystemExit("the new patch doesn't rebuild the new file")
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_bytes(out)
    print(f"{a.out}: {len(out)} bytes ({len(triples)} runs; {sum(t[1] for t in triples)} literal bytes); "
          f"new sha1 {hashlib.sha1(new).hexdigest()}")
    return 0


def cmd_apply(a) -> int:
    new = apply(Path(a.rom).read_bytes(), Path(a.patch).read_bytes())
    Path(a.out).write_bytes(new)
    print(f"{a.out}: sha1 {hashlib.sha1(new).hexdigest()}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--old", required=True)
    m.add_argument("--new", required=True)
    m.add_argument("--bsdiff")
    m.add_argument("--out", default=str(DEFAULT_PATCH))
    m.set_defaults(fn=cmd_make)
    ap = sub.add_parser("apply")
    ap.add_argument("rom")
    ap.add_argument("out")
    ap.add_argument("--patch", default=str(DEFAULT_PATCH))
    ap.set_defaults(fn=cmd_apply)
    a = p.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
