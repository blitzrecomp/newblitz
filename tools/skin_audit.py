#!/usr/bin/env python3
"""Player skin tone audit for the 2026 roster pack (packs/blitz2026).

  skin_audit.py worksheet [--rom build/teamplus.z64] [--pack packs/blitz2026] [--out build/skin_audit.csv]
      Write a worksheet: every player slot, the 2026 player in it, the 1999 player whose record it
      replaced, the skin value the game currently shows (inherited from that 1999 player, unless the
      pack sets one), an empty "set_skin" column and a link to the player's ESPN page.
  skin_audit.py import SOURCE.csv [--threshold 4 | --shades --max 7] [--out build/skin_audit.csv]
      Fill the worksheet's set_skin from a source table instead of by hand: a CSV with columns
      team (franchise id or nickname), last (or name), number (optional) and tone (a 0-7 scale such
      as Madden's skinTone, or 0/1 with 1 = darker). tone >= threshold means darker, which is the game's 0
      (in Blitz, 1 is the lighter texture: Marino, Favre and Manning are 1; Barry Sanders and Moss 0).
      --shades spreads a 0..MAX scale (lightest first) over the six shades instead: 5 Lighter, 1 Light,
      2 Light-medium, 3 Medium, 4 Medium-dark, 0 Darker. Rows that don't match exactly one 2026 player are listed, not
      guessed. Review, then run apply.
      Source ideas (docs in the module): Madden 27 franchise files via the madden-franchise library.
  skin_audit.py apply WORKSHEET.csv [--pack packs/blitz2026]
      Copy the filled-in "set_skin" values (0-5) into the pack's players ("skin"). Rows left empty
      are untouched.

Skin values (team+ player record +5), light to dark: 5 Lighter, 1 Light, 2 Light-medium, 3 Medium,
4 Medium-dark, 0 Darker. 1 and 0 are the game's own two texture sets; the others are drawn by
src/game/skin_tones.cpp. The pack set none
at first, so every 2026 player showed whatever the 1999 player in that slot had. This tool never chooses a
value: they come from the person filling in the worksheet (from a source they trust), never from a name or
photo. Check what the shades look like in the game (the 1999 players in the worksheet are a reference) before
filling it in. The worksheet goes in build/ and stays out of git; the values end up in pack.json.
"""
import argparse
import csv
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from roster_pack import NAMES, dump  # noqa: E402

BLOCKS, BLOCK_SIZE, PLAYER_SIZE, SLOTS = 0x11F17C, 0x220, 0x18, 15
OLD_NICKNAMES = {"Redskins": "WAS", "Commanders": "WAS"}  # team+ still says Redskins
FIELDS = ["franchise", "slot", "number", "player_2026", "replaced_1999", "skin_now", "set_skin", "espn_page"]


def franchise_of(nick):
    if nick in OLD_NICKNAMES:
        return OLD_NICKNAMES[nick]
    for f, n in NAMES.items():
        if n == nick:
            return f
    return None


def rom_rosters(rom_path):
    rom = Path(rom_path).read_bytes()
    out = {}
    for t in range(40):
        b = BLOCKS + t * BLOCK_SIZE
        if rom[b] & 0x80:
            break
        f = franchise_of(rom[b + 0x1E0:b + 0x1F0].split(b"\0")[0].decode("latin1"))
        if f is None:
            continue
        players = []
        for s in range(SLOTS):
            r = b + s * PLAYER_SIZE
            players.append({"skin": rom[r + 5], "number": (rom[r + 6] >> 4) * 10 + (rom[r + 6] & 15),
                            "last": rom[r + 8:r + 0x18].split(b"\0")[0].decode("latin1")})
        out[f] = players
    return out


def espn_pages():
    """franchise -> {number: player page}, from ESPN's roster JSON (tools/roster_fetch.py); {} offline."""
    try:
        import roster_fetch
        ids = roster_fetch.team_ids()
        pages = {}
        for f, tid in ids.items():
            pages[f] = {p["number"]: f"https://www.espn.com/nfl/player/_/id/{p['id']}"
                        for p in roster_fetch.roster(tid).values() if p["number"] is not None}
        return pages
    except Exception as e:
        print(f"(no ESPN links: {e})")
        return {}


def cmd_worksheet(a):
    pack = json.loads((Path(a.pack) / "pack.json").read_text())
    rom = rom_rosters(a.rom)
    pages = espn_pages()
    rows = []
    for team in pack["teams"]:
        f = team["franchise"]
        old = rom.get(f, [{}] * SLOTS)
        by_slot = {p["slot"]: p for p in team.get("players", [])}
        for s in range(SLOTS):
            p, o = by_slot.get(s, {}), old[s] if s < len(old) else {}
            rows.append({
                "franchise": f, "slot": s, "number": p.get("number", o.get("number", "")),
                "player_2026": p.get("last", ""), "replaced_1999": o.get("last", "") or f"#{o.get('number', '?')}",
                "skin_now": p.get("skin", o.get("skin", "")), "set_skin": "",
                "espn_page": pages.get(f, {}).get(p.get("number"), "") if p.get("last") else "",
            })
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    named = sum(1 for r in rows if r["player_2026"])
    print(f"wrote {out}: {len(rows)} slots ({named} with a named 2026 player). Fill in set_skin (0-5: 5 Lighter, 1 Light, 2 Light-medium, 3 Medium, 4 Medium-dark, 0 Darker), then run apply.")


def cmd_apply(a):
    pack_path = Path(a.pack) / "pack.json"
    pack = json.loads(pack_path.read_text())
    teams = {t["franchise"]: t for t in pack["teams"]}
    changed = 0
    with open(a.worksheet, newline="") as fh:
        for row in csv.DictReader(fh):
            v = (row.get("set_skin") or "").strip()
            if v == "":
                continue
            if v not in ("0", "1", "2", "3", "4", "5"):
                raise SystemExit(f"{row['franchise']} slot {row['slot']}: set_skin must be 0-5, not {v!r}")
            team = teams.get(row["franchise"])
            if team is None:
                raise SystemExit(f"unknown franchise {row['franchise']}")
            slot = int(row["slot"])
            players = {p["slot"]: p for p in team.setdefault("players", [])}
            p = players.get(slot)
            if p is None:
                p = {"slot": slot}
                team["players"].append(p)
                team["players"].sort(key=lambda q: q["slot"])
            if p.get("skin") != int(v):
                p["skin"] = int(v)
                changed += 1
    dump(pack, pack_path)
    print(f"{pack_path}: {changed} skin values set")


# The six shades from lightest to darkest, as pack values.
SHADES = [5, 1, 2, 3, 4, 0]


def tone_value(tone, threshold=4, scale_max=None):
    """A source's tone (lightest = 0) as a pack skin value. With scale_max, the 0..scale_max scale is spread
    over the six shades; otherwise tone >= threshold is Darker (0) and anything lighter is Light (1)."""
    if scale_max:
        # Six equal bins over the 0..scale_max steps.
        step = min(5, int(min(max(tone, 0), scale_max) * 6 // (scale_max + 1)))
        return SHADES[step]
    dark = tone >= threshold if threshold > 1 else tone >= 1
    return 0 if dark else 1  # the game's value: 1 = the lighter texture, 0 = the darker


def cmd_import(a):
    sys.path.insert(0, str(ROOT / "tools"))
    from roster_pack import franchise_id
    rows = list(csv.DictReader(open(a.out, newline="")))
    by_team = {}
    for r in rows:
        if r["player_2026"]:
            by_team.setdefault(r["franchise"], []).append(r)
    def norm(n):
        words = [w for w in n.replace("-", " ").split() if w.rstrip(".").lower() not in ("jr", "sr", "ii", "iii", "iv", "v")]
        return "".join(ch for ch in " ".join(words).lower() if ch.isalnum())
    matched, problems = 0, []
    for src in csv.DictReader(open(a.source, newline="")):
        try:
            f = franchise_id((src.get("team") or "").strip())
        except SystemExit:
            problems.append(f"unknown team {src.get('team')!r}")
            continue
        last = norm(src.get("last") or src.get("name") or "")
        number = (src.get("number") or "").strip()
        cands = [r for r in by_team.get(f, []) if norm(r["player_2026"]) == last]
        if number and len(cands) > 1:
            cands = [r for r in cands if r["number"] == number]
        if not cands and number and last:
            # Same team and number, one name inside the other (sources split hyphenated names: "GARDNER"
            # for Gardner-Johnson, "SHAAIR" for Al-Shaair).
            cands = [r for r in by_team.get(f, []) if r["number"] == number and
                     (last in norm(r["player_2026"]) or norm(r["player_2026"]) in last)]
        if len(cands) != 1:
            continue  # source covers whole rosters; only our 15 per team matter
        cands[0]["set_skin"] = str(tone_value(float(src["tone"]), a.threshold, a.max if a.shades else None))
        matched += 1
    with open(a.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    missing = [f"{r['franchise']} #{r['number']} {r['player_2026']}" for rs in by_team.values() for r in rs if not r["set_skin"]]
    print(f"{matched} players filled from {a.source}; {len(missing)} still empty: {', '.join(missing[:20])}{' ...' if len(missing) > 20 else ''}")
    for p in problems[:10]:
        print(p)
    print(f"Review {a.out}, then: skin_audit.py apply {a.out}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("worksheet")
    w.add_argument("--rom", default=str(ROOT / "build" / "teamplus.z64"))
    w.add_argument("--pack", default=str(ROOT / "packs" / "blitz2026"))
    w.add_argument("--out", default=str(ROOT / "build" / "skin_audit.csv"))
    i = sub.add_parser("import")
    i.add_argument("source")
    i.add_argument("--threshold", type=int, default=4)
    i.add_argument("--shades", action="store_true", help="spread the tone scale over the six shades")
    i.add_argument("--max", type=float, default=7, help="the source scale's darkest value (with --shades)")
    i.add_argument("--out", default=str(ROOT / "build" / "skin_audit.csv"))
    p = sub.add_parser("apply")
    p.add_argument("worksheet")
    p.add_argument("--pack", default=str(ROOT / "packs" / "blitz2026"))
    a = ap.parse_args(argv)
    {"worksheet": cmd_worksheet, "import": cmd_import, "apply": cmd_apply}[a.cmd](a)


if __name__ == "__main__":
    main()
