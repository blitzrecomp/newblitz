#!/usr/bin/env python3
"""Decode NFL Blitz 2000 offense plays (formation + per-player route scripts) from the ROM.

Analysis only (see docs/notes/PLAY_ART.md). Reads build/baserom.z64 (or the path given with --rom).

  play_routes.py                 list every offense play with each player's decoded route
  play_routes.py "QB Post"       one play
  play_routes.py --json OUT      write all plays as JSON (offense frame, yards)

Offense frame: D = yards downfield from the line of scrimmage (negative = backfield),
L = yards sideways, + = the offense's right (screen right with the camera behind the offense); the formation's z
offset for side 0, play not flipped. Paths apply the left-of-the-ball mirror.
"""
import argparse
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UNITS_PER_YARD = 7.8           # 0x40F9999A in the lineup code (0x803028EC): x = spot*7.8 - 390

# Segments (tools/gen_syms.py): main ROM 0x2FC20 -> 0x80248F00, ovl_a ROM 0x93EC0 -> 0x802EFE70.
MAIN = (0x2FC20, 0x80248F00, 0x93EC0)
OVL_A = (0x93EC0, 0x802EFE70, 0xDE350)

OFFENSE_PTRS = 0x80336B24       # 45 play-record pointers (ovl_a), index-aligned with...
OFFENSE_NAMES = 0x80336BD8      # ...45 name-string pointers
N_OFFENSE = 45

# Route script opcodes, interpreter ovl_a 0x80310B98, jump table 0x80338688 (op-1).
# name, size in words, ends the scripted path?
OPS = {
    1: ("NOP", 1, False),
    2: ("TURN", 2, False),         # arg: direction, 1024 = full turn, 0 = straight downfield; negated when flipped
    3: ("RUN_TO_LOS", 2, False),   # run until arg yards past the line of scrimmage ("reaches los+%d")
    4: ("RUN_YDS", 2, False),      # run arg yards in the current direction ("begins/finishes RUN_YDS(%d)")
    5: ("IMPROVISE", 1, True),     # "begins improvising": free AI from here
    6: ("MODE3", 1, True),
    7: ("MODE3", 1, True),
    8: ("BLOCK", 1, True),         # "goes into BLOCK mode"
    9: ("WAIT_A", 1, False),
    10: ("COVER_HINT", 4, False),  # dir, chance, time: tells the covering defender where the break goes
    11: ("FLAG_ON", 1, False),
    12: ("FLAG_OFF", 1, False),
    13: ("WAIT_B", 1, False),
    14: ("WAIT_C", 1, False),
    15: ("COVER_HINT2", 3, False), # dir, delay: another hint written into the covering defender's drone block
    16: ("WAIT_TICKS", 2, False),
    20: ("SET_68", 2, False),
}


class Rom:
    def __init__(self, path):
        self.b = Path(path).read_bytes()

    def off(self, a):
        for rom, vram, rom_end in (MAIN, OVL_A):
            if vram <= a < vram + (rom_end - rom):
                return rom + (a - vram)
        raise ValueError(hex(a))

    def i32(self, a):
        return struct.unpack(">i", self.b[self.off(a):self.off(a) + 4])[0]

    def u32(self, a):
        return self.i32(a) & 0xFFFFFFFF

    def f32(self, a):
        return struct.unpack(">f", self.b[self.off(a):self.off(a) + 4])[0]

    def cstr(self, a):
        o = self.off(a)
        return self.b[o:self.b.index(b"\0", o)].decode("latin-1")


def decode_script(rom, ptr, limit=64):
    ops = []
    a = ptr
    for _ in range(limit):
        op = rom.i32(a)
        if op not in OPS:
            ops.append(("UNKNOWN", op))
            break
        name, size, end = OPS[op]
        ops.append((name, *[rom.i32(a + 4 * k) for k in range(1, size)]))
        a += 4 * size
        if end:
            break
    return ops


def trace(start, ops, flipped=False):
    """Follow TURN/RUN ops from the lineup spot. Returns (points, ending)."""
    d, l = start
    pts = [(d, l)]
    ang = 0
    ending = "end"
    for op in ops:
        n = op[0]
        if n == "TURN":
            ang = -op[1] if flipped else op[1]
        elif n in ("RUN_YDS", "RUN_TO_LOS"):
            t = ang * 2 * math.pi / 1024
            dd, dl = math.cos(t), -math.sin(t)
            if n == "RUN_YDS":
                dist = op[1]
            else:
                if dd <= 1e-3:
                    continue
                dist = max(0.0, (op[1] - d) / dd)
            d, l = d + dd * dist, l + dl * dist
            pts.append((round(d, 2), round(l, 2)))
        elif n in ("IMPROVISE", "BLOCK", "MODE3", "UNKNOWN"):
            ending = n.lower()
    return pts, ending


def load_plays(rom):
    plays = []
    for i in range(N_OFFENSE):
        rec = rom.u32(OFFENSE_PTRS + 4 * i)
        name = rom.cstr(rom.u32(OFFENSE_NAMES + 4 * i))
        form = rom.u32(rec)
        flags = rom.u32(rec + 4)
        players = []
        for s in range(7):
            fe = form + 0x14 * s
            fx, fz = rom.f32(fe), rom.f32(fe + 4)
            mode, fflags = rom.u32(fe + 0xC), rom.u32(fe + 0x10)
            sp = rom.u32(rec + 8 + 4 * s)
            ops = decode_script(rom, sp)
            start = (round(fx / UNITS_PER_YARD, 2), round(fz / UNITS_PER_YARD, 2))
            # Players lined up left of the ball run their script mirrored (player +0x1A4 bit 0x200 negates every
            # direction; the rule "left of the ball" is inferred from the play-select tiles, see PLAY_ART.md).
            mirrored = fz < 0
            pts, ending = trace(start, ops, flipped=mirrored)
            players.append(dict(slot=s, x=fx, z=fz, mode=mode, flags=fflags, script=hex(sp), mirrored=mirrored,
                                ops=[list(o) for o in ops], path=pts, ending=ending))
        plays.append(dict(index=i, name=name, record=hex(rec), formation=hex(form), flags=flags, players=players))
    return plays


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name", nargs="?")
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    ap.add_argument("--json")
    a = ap.parse_args()
    plays = load_plays(Rom(a.rom))
    if a.json:
        Path(a.json).write_text(json.dumps(plays, indent=1))
    for p in plays:
        if a.name and p["name"].lower() != a.name.lower():
            continue
        print(f"== {p['index']:2d} {p['name']}  record {p['record']} formation {p['formation']} flags {p['flags']:#x}")
        for pl in p["players"]:
            path = " -> ".join(f"({d:+.1f},{l:+.1f})" for d, l in pl["path"])
            m = " mirrored" if pl["mirrored"] else ""
            print(f"  slot {pl['slot']} mode {pl['mode']} flags {pl['flags']:#05x} script {pl['script']}{m}: {path} [{pl['ending']}]")
            if a.name:
                print("      " + " ".join(o[0] + ("(" + ",".join(str(x) for x in o[1:]) + ")" if len(o) > 1 else "") for o in pl["ops"]))


if __name__ == "__main__":
    main()
