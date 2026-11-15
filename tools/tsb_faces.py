#!/usr/bin/env python3
"""Dump every player's name, jersey number and face from a Tecmo Super Bowl (NES) ROM.

  tsb_faces.py ROM.nes [--out build/tsb_faces.csv] [--layout auto|nes28|cxrom105|cxrom111] [--force]

Writes a CSV with columns team, position, first, last, number, face, tone:
  team      franchise id (ARI ... WAS), mapped from the ROM's team slot
  position  TSB roster slot (QB1, QB2, RB1 ... K, P)
  first     first name as stored (TSB keeps it lower case), last name as stored (upper case)
  number    jersey number (stored as BCD; a non-BCD byte is written as 0xNN)
  face      face byte, hex (0x00-0xD4)
  tone      0 if face < 0x53 (lighter faces) else 1 (darker faces): the TSB editors' rule
The CSV fits `skin_audit.py import` (team/last/number/tone).

Meant for SBlueman's "Tecmo Super Bowl 2026" (32 teams). Modern 32-team TSB hacks are built on
cxrom's expanded 32-team NES ROM ("CXROM"), which TSBTool Supreme reads; SBlueman's 18-week base
("SbluemanBase_18week_RealNFL") is a CXROM v1.05-size ROM. The original 28-team ROM works too.

Layout (ported, read-only, from BAD-AL's TSBTool Supreme, https://github.com/BAD-AL/tsbtools at
commit 85f5f50 (2026-09-30), TSBProjects/TSBTool/Core/; same logic in the Java port
BAD-AL/Android-TSBToolSupreme2 app/src/main/java/com/example/tsbtoolsupreme/):

  ROM type (TecmoToolFactory.cs l.11-15, CheckRomType l.120-166): by file size.
    0x60010 original 28-team NES; 0x80010 CXROM v1.05 (also the 18-week variant, same player
    layout); 0xC0010 CXROM v1.11 (the factory falls back to v1.05 offsets when the v1.11 ones
    do not give a name for 49ers QB1, l.53-59; we do the same with a plausibility check).
  Team order (TecmoToolFactory.cs l.40-50 for CXROM, l.100-107 for the original):
    CXROM: 28 teams in slots 0-27, then AFC, NFC (Pro Bowl, slots 28-29, no own roster data),
    then 49ers, rams, seahawks, cardinals (slots 30-33, the "expansion" teams).
  Positions (TecmoTool.cs l.54-58): 30 slots, QB1 ... K, P.
  Names and numbers (TecmoTool.cs GetName l.466-527, GetDataPosition l.813-833,
    GetNextDataPosition l.841-866; CXRomTSBTool.cs SetupForCxROM l.97-111,
    GetDataPosition l.683-716, GetNextDataPosition l.724-754):
    A table of little-endian 16-bit pointers, one per player in team-major order plus one end
    pointer. A record is [jersey byte][ASCII name], running to the next record's pointer. The
    name is "first" in lower case followed by "LAST" in upper case, split at the first byte in
    'A'-'Z' (position 1 when there is none). File offset = pointer + delta.
      original:  table 0x48, 28*30+1 pointers (end pointer at 0x6D8), delta -0x8000+0x10
      CXROM:     table 0x54 (0x48+12), teams 0-27, end pointer at 0x6E4, delta -0x8000+0x10
                 expansion teams 30-33: their own table, 4*30+1 pointers (0x3C bytes per team)
                 at 0x3EB0 (v1.05) / 0x3E054 (v1.11), delta 0x30010 (v1.05) / 0x36010 (v1.11)
    Jersey byte is printed by TSBTool as hex ("#{0:x}"), i.e. BCD.
  Faces (TecmoTool.cs GetFace l.1917-1930, faceOffsets l.1903-1906; CXRomTSBTool.cs GetFace
    l.886-905): byte at 0x3012 + faceOffsets[position] + team * 0x75 (= the player's attribute
    record + 2). CXROM expansion teams use team - 2 (so the 32 real teams fill 0x3010..0x3EB0).
"""
import argparse
import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

INES_MAGIC = b"NES\x1a"
HEADER_LEN = 0x10

LEN_NES28 = 0x60010
LEN_CXROM_V105 = 0x80010
LEN_CXROM_V111 = 0xC0010

POSITIONS = ["QB1", "QB2", "RB1", "RB2", "RB3", "RB4", "WR1", "WR2", "WR3", "WR4", "TE1",
             "TE2", "C", "LG", "RG", "LT", "RT",
             "RE", "NT", "LE", "ROLB", "RILB", "LILB", "LOLB", "RCB", "LCB", "FS", "SS", "K", "P"]
PER_TEAM = len(POSITIONS)  # 30

# Attribute/face table: record start of each position within a team's 0x75-byte block.
FACE_BASE = 0x3012  # 0x3010 attribute table + 2
TEAM_STRIDE = 0x75
FACE_OFFSETS = [0x00, 0x05, 0x0A, 0x0E, 0x12, 0x16, 0x1A, 0x1E, 0x22, 0x26, 0x2A,
                0x2E, 0x32, 0x35, 0x38, 0x3B, 0x3E, 0x41, 0x45, 0x49, 0x4D, 0x51,
                0x55, 0x59, 0x5D, 0x61, 0x65, 0x69, 0x6D, 0x71]
FACE_MAX = 0xD4
DARK_FACE_FROM = 0x53  # editor rule: face < 0x53 -> 0 (light), else 1 (dark)

BANK0_DELTA = -0x8000 + 0x10

# ROM team slot -> franchise id. None = slot without a roster (Pro Bowl).
TEAMS_NES28 = ["BUF", "IND", "MIA", "NE", "NYJ", "CIN", "CLE", "TEN",  # oilers -> TEN
               "PIT", "DEN", "KC", "LV", "LAC", "SEA", "WAS", "NYG",
               "PHI", "ARI", "DAL", "CHI", "DET", "GB", "MIN", "TB",
               "SF", "LAR", "NO", "ATL"]
TEAMS_CXROM = ["BUF", "MIA", "NE", "NYJ",
               "CIN", "CLE", "BAL", "PIT",
               "IND", "HOU", "JAX", "TEN",
               "DEN", "KC", "LV", "LAC",
               "WAS", "NYG", "PHI", "DAL",
               "CHI", "DET", "GB", "MIN",
               "TB", "NO", "ATL", "CAR",
               None, None,  # AFC, NFC
               "SF", "LAR", "SEA", "ARI"]

# name table: (pointer table offset, first team slot, number of teams, file delta)
LAYOUTS = {
    "nes28": {
        "desc": "original 28-team NES",
        "teams": TEAMS_NES28,
        "tables": [(0x48, 0, 28, BANK0_DELTA)],
    },
    "cxrom105": {
        "desc": "CXROM 32-team v1.05 / 18-week",
        "teams": TEAMS_CXROM,
        "tables": [(0x54, 0, 28, BANK0_DELTA), (0x3EB0, 30, 4, 0x30010)],
    },
    "cxrom111": {
        "desc": "CXROM 32-team v1.11",
        "teams": TEAMS_CXROM,
        "tables": [(0x54, 0, 28, BANK0_DELTA), (0x3E054, 30, 4, 0x36010)],
    },
}
NAME_CHARS = set(b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ .'-")
MAX_NAME = 16


class LayoutError(Exception):
    pass


def load_rom_bytes(rom, name="ROM"):
    """Return the image with its 16-byte iNES header (added back to a headerless dump)."""
    if rom[:4] != INES_MAGIC and len(rom) + HEADER_LEN in (LEN_NES28, LEN_CXROM_V105, LEN_CXROM_V111):
        return bytes(HEADER_LEN) + rom  # offsets below assume the header is there
    if rom[:4] != INES_MAGIC:
        raise LayoutError(f"{name}: not an iNES (.nes) file")
    return rom


def load_rom(path):
    return load_rom_bytes(Path(path).read_bytes(), path)


def detect_layout(rom):
    """Candidate layouts by file size, most likely first (TecmoToolFactory.CheckRomType)."""
    n = len(rom)
    if n == LEN_NES28:
        return ["nes28"]
    if n == LEN_CXROM_V105:
        return ["cxrom105"]
    if n == LEN_CXROM_V111:
        return ["cxrom111", "cxrom105"]
    raise LayoutError(
        f"ROM is 0x{n:X} bytes; expected 0x{LEN_NES28:X} (original 28-team TSB), 0x{LEN_CXROM_V105:X} "
        f"(32-team CXROM v1.05 / 18-week) or 0x{LEN_CXROM_V111:X} (CXROM v1.11). "
        "Use --layout to force one.")


def u16(rom, at):
    return rom[at] | (rom[at + 1] << 8)


def split_name(raw):
    """TecmoTool.GetName: last name starts at the first 'A'-'Z' byte (index 1 when there is none)."""
    if not raw:
        return "", ""
    split = next((i for i, b in enumerate(raw) if 65 <= b <= 90), 1)
    text = raw.decode("latin1")
    return text[:split], text[split:]


def bcd(byte):
    hi, lo = byte >> 4, byte & 0xF
    return str(hi * 10 + lo) if hi <= 9 and lo <= 9 else f"0x{byte:02X}"


def face_offset(team_slot, pos_index, layout):
    t = team_slot - 2 if layout != "nes28" and team_slot >= 30 else team_slot
    return FACE_BASE + FACE_OFFSETS[pos_index] + t * TEAM_STRIDE


def read_players(rom, layout):
    """Yield one dict per player: team, position, first, last, number, face, tone, ok."""
    spec = LAYOUTS[layout]
    for table, first_team, count, delta in spec["tables"]:
        end_ptr = table + 2 * count * PER_TEAM
        if end_ptr + 2 > len(rom):
            raise LayoutError(f"{layout}: name pointer table 0x{table:X} runs past the end of the ROM")
        for k in range(count * PER_TEAM):
            slot, pi = first_team + k // PER_TEAM, k % PER_TEAM
            start = u16(rom, table + 2 * k) + delta
            nxt = u16(rom, table + 2 * k + 2) + delta
            ok = 0 <= start < nxt <= len(rom) and nxt - start - 1 <= MAX_NAME
            raw = rom[start + 1:nxt] if ok else b""
            ok = ok and bool(raw) and all(b in NAME_CHARS for b in raw)
            first, last = split_name(raw)
            face_at = face_offset(slot, pi, layout)
            face = rom[face_at]
            yield {
                "team": spec["teams"][slot],
                "position": POSITIONS[pi],
                "first": first,
                "last": last,
                "number": bcd(rom[start]) if 0 <= start < len(rom) else "",
                "face": f"0x{face:02X}",
                "tone": 0 if face < DARK_FACE_FROM else 1,
                "ok": ok and face <= FACE_MAX,
            }


def decode(rom, layout=None):
    """(layout, players). Auto-detects; tries fallbacks and keeps the first whose names look right."""
    candidates = [layout] if layout else detect_layout(rom)
    best = None
    for cand in candidates:
        try:
            players = list(read_players(rom, cand))
        except LayoutError:
            if layout:
                raise
            continue
        bad = sum(not p["ok"] for p in players)
        if best is None or bad < best[2]:
            best = (cand, players, bad)
        if bad <= len(players) // 10:
            break
    if best is None:
        raise LayoutError("no layout could be read from this ROM")
    return best[0], best[1]


FIELDS = ["team", "position", "first", "last", "number", "face", "tone"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--out", default=str(ROOT / "build" / "tsb_faces.csv"))
    ap.add_argument("--layout", choices=sorted(LAYOUTS), help="skip size detection")
    ap.add_argument("--force", action="store_true", help="write the CSV even if many records look wrong")
    a = ap.parse_args(argv)
    try:
        rom = load_rom(a.rom)
        layout, players = decode(rom, a.layout)
    except (LayoutError, OSError) as e:
        sys.exit(f"tsb_faces: {e}")
    bad = [p for p in players if not p["ok"]]
    print(f"{a.rom}: {LAYOUTS[layout]['desc']} layout, {len(players)} players, "
          f"{len({p['team'] for p in players})} teams, {len(bad)} suspicious records")
    for p in bad[:10]:
        print(f"  suspicious: {p['team']} {p['position']} {p['first']!r} {p['last']!r} #{p['number']} face {p['face']}")
    if len(bad) > len(players) // 10 and not a.force:
        sys.exit("tsb_faces: too many suspicious records; the layout is probably wrong for this ROM "
                 "(try --layout, or --force to write anyway)")
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=FIELDS, extrasaction="ignore")
        w.writeheader()
        w.writerows(players)
    dark = sum(p["tone"] for p in players)
    print(f"wrote {out} ({dark} tone=1, {len(players) - dark} tone=0)")


if __name__ == "__main__":
    main()
