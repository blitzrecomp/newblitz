#!/usr/bin/env python3
"""Refresh the Blitz 2026 roster pack (packs/blitz2026) from new research, keeping everything else.

  roster_pack.py build RAW.json PACKDIR [--version V] [--no-checks]
      Merge a roster research result (tools/roster_refresh/rosters.workflow.js) into PACKDIR/pack.json:
      each team's ratings and player slots are replaced; the pack id, order, names, abbreviations
      and season stay. Fact-check corrections in RAW["check"] are applied unless --no-checks.
      Prints a changelog, validates with tools/pack.py's rules and sets "version" (default: today).
  roster_pack.py season SCHEDULE.json PACKDIR
      Replace the season's schedule and year (tools/roster_refresh/schedule_prompt.md).
  roster_pack.py diff OLD.json NEW.json
      The changelog between two pack.json files.
  roster_pack.py edits EDITS PACKDIR [--version V]
      Apply Roster Room edits (the roster editor page): EDITS is a folder of <FRANCHISE>.json files
      as the page saves them ({"team": {...}}), or a pack.json the page exported. Each edited team
      replaces the pack's team; the id, order, season and other teams stay. Validates and prints a
      changelog. Skin shades picked there are also remembered by ESPN player id in PACKDIR/picks.json
      (--no-espn skips the lookup), so a roster update keeps a player's shade after a trade or a new
      number.

Keep the pack id: saves are kept per pack id, so a new id would hide the existing seasons. Renamed
players get no announcer call (pack.py's default), so the announcer never says an old name.
"""
import argparse
import datetime
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pack as packtool  # noqa: E402


NAMES = {
    "ARI": "Cardinals", "ATL": "Falcons", "BAL": "Ravens", "BUF": "Bills", "CAR": "Panthers", "CHI": "Bears",
    "CIN": "Bengals", "CLE": "Browns", "DAL": "Cowboys", "DEN": "Broncos", "DET": "Lions", "GB": "Packers",
    "HOU": "Texans", "IND": "Colts", "JAX": "Jaguars", "KC": "Chiefs", "LV": "Raiders", "LAC": "Chargers",
    "LAR": "Rams", "MIA": "Dolphins", "MIN": "Vikings", "NE": "Patriots", "NO": "Saints", "NYG": "Giants",
    "NYJ": "Jets", "PHI": "Eagles", "PIT": "Steelers", "SF": "49ers", "SEA": "Seahawks", "TB": "Buccaneers",
    "TEN": "Titans", "WAS": "Commanders",
}


def franchise_id(name):
    """"HOU", "Houston Texans" or "Texans" -> "HOU"."""
    if name in NAMES:
        return name
    for f, nick in NAMES.items():
        if nick.lower() in name.lower():
            return f
    raise SystemExit(f"unknown team '{name}'")


def load(path):
    p = Path(path)
    return json.loads((p / "pack.json" if p.is_dir() else p).read_text())


def dump(pack, path):
    """indent=1 like the original, with each schedule week on one line."""
    sched = pack.get("season", {}).get("schedule")
    if sched is not None:
        pack = dict(pack, season=dict(pack["season"], schedule="@SCHED@"))
    text = json.dumps(pack, indent=1, ensure_ascii=False)
    if sched is not None:
        weeks = ",\n".join("   [" + ", ".join(f'["{a}", "{h}"]' for a, h in w) + "]" for w in sched)
        text = text.replace('"@SCHED@"', "[\n" + weeks + "\n  ]")
    json.loads(text)
    Path(path).write_text(text + "\n")


def changelog(old, new):
    lines = []
    old_teams = {t["franchise"]: t for t in old.get("teams", [])}
    for t in new.get("teams", []):
        o = old_teams.get(t["franchise"], {})
        if o.get("ratings") != t.get("ratings"):
            lines.append(f"{t['franchise']}: ratings {o.get('ratings')} -> {t.get('ratings')}")
        op = {p["slot"]: p for p in o.get("players", [])}
        for p in t.get("players", []):
            q = op.get(p["slot"], {})
            if (q.get("last"), q.get("number")) != (p.get("last"), p.get("number")):
                was = f"{q.get('last', '')} #{q.get('number', '?')}".strip()
                now = f"{p.get('last', '')} #{p.get('number', '?')}".strip()
                lines.append(f"{t['franchise']} slot {p['slot']}: {was} -> {now}")
            for k in ("skin", "heavy", "size"):
                if q.get(k) != p.get(k):
                    lines.append(f"{t['franchise']} slot {p['slot']} {k}: {q.get(k)} -> {p.get(k)}")
        for k in ("abbr", "city", "select", "nickname"):
            if o.get(k) != t.get(k):
                lines.append(f"{t['franchise']} {k}: {o.get(k)} -> {t.get(k)}")
    return lines


def cmd_build(a):
    raw = json.loads(Path(a.raw).read_text())
    pack_path = Path(a.packdir) / "pack.json"
    old = json.loads(pack_path.read_text())
    new = json.loads(json.dumps(old))
    by_franchise = {t["franchise"]: t for t in new["teams"]}
    fixes = 0
    research = {franchise_id(t["franchise"]): t for t in raw["teams"]}
    picks = load_picks(a.packdir)
    if not a.no_checks:
        for c in raw.get("check", []):
            if c.get("correct") or not c.get("fix"):
                continue
            m = re.match(r"\s*(.+?)\s*#\s*(\d+)\s*$", c["fix"])
            t = research.get(franchise_id(c["franchise"]))
            if not m or t is None:
                print(f"skipped check fix {c}", file=sys.stderr)
                continue
            for p in t["players"]:
                if p["slot"] == c["slot"]:
                    p["last"], p["number"] = m.group(1), int(m.group(2))
                    fixes += 1
    for f, t in research.items():
        if f not in by_franchise:
            raise SystemExit(f"{f}: not a team in {pack_path}")
        team = by_franchise[f]
        team["ratings"] = {k: int(v) for k, v in t["ratings"].items()}
        players = {p["slot"]: p for p in team.get("players", [])}
        for p in t["players"]:
            if not 0 <= p["slot"] <= 14:
                continue
            entry = {"slot": p["slot"], "number": int(p["number"])}
            if p.get("last"):
                entry["last"] = p["last"]
            pick = picks.get(str(p.get("espn", "")))
            # The same player as before keeps what the research doesn't cover (skin tone, size, ...).
            was = players.get(p["slot"], {})
            if (was.get("number"), was.get("last")) == (entry["number"], entry.get("last")):
                entry = dict(was, **entry)
            if "heavy" in p:
                entry["heavy"] = int(p["heavy"])
            if pick:  # a shade picked by hand follows the player (picks.json)
                entry["skin"] = pick["skin"]
            players[p["slot"]] = entry
        team["players"] = [players[k] for k in sorted(players)]
    new["version"] = a.version or datetime.date.today().strftime("%Y.%m.%d")
    packtool.validate_v2(new)
    for line in changelog(old, new):
        print(line)
    dump(new, pack_path)
    print(f"{pack_path}: {len(research)} teams refreshed, {fixes} fact-check fixes, version {new['version']}")


def cmd_season(a):
    sched = json.loads(Path(a.schedule).read_text())
    pack_path = Path(a.packdir) / "pack.json"
    pack = json.loads(pack_path.read_text())
    pack["season"]["year"] = int(sched["season"])
    pack["season"]["schedule"] = sched["weeks"]
    print("\n".join(packtool.validate_season(pack["season"])))
    dump(pack, pack_path)
    print(f"{pack_path}: season {pack['season']['year']} schedule installed")


def load_picks(packdir):
    """PACKDIR/picks.json: shades picked by hand, by ESPN athlete id ({"<id>": {"skin": n, "who": "LAR Stafford #9"}})."""
    path = Path(packdir) / "picks.json"
    return json.loads(path.read_text()) if path.exists() else {}


def remember_picks(teams, packdir):
    """Every skin shade on the edited teams (the page shows them all, so each is a choice), looked up on ESPN's
    rosters by team, number and name."""
    import roster_fetch
    changed = [(f, p) for f, t in teams.items() for p in t.get("players", []) if "skin" in p]
    if not changed:
        return
    picks, ids, rosters, missed = load_picks(packdir), roster_fetch.team_ids(), {}, []
    for f, p in changed:
        if f not in ids:
            missed.append(f"{f} {p.get('last')}")
            continue
        players = rosters.setdefault(f, roster_fetch.roster(ids[f]))
        last = roster_fetch.clean_last(p.get("last", "")).lower()
        match = [q for q in players.values() if q["number"] == p.get("number") and (not last or q["last"].lower() == last)]
        match = match or [q for q in players.values() if last and q["last"].lower() == last]
        if len(match) != 1:
            missed.append(f"{f} {p.get('last')} #{p.get('number')}")
            continue
        picks[match[0]["id"]] = {"skin": p["skin"], "who": f"{f} {p.get('last', '')} #{p.get('number')}"}
    (Path(packdir) / "picks.json").write_text(json.dumps(dict(sorted(picks.items())), indent=1) + "\n")
    print(f"picks.json: {len(changed) - len(missed)} shades remembered by ESPN id" + (f"; not found on ESPN: {', '.join(missed)}" if missed else ""))


def cmd_edits(a):
    pack_path = Path(a.packdir) / "pack.json"
    old = json.loads(pack_path.read_text())
    new = json.loads(json.dumps(old))
    src = Path(a.edits)
    if src.is_file():  # an exported pack.json: take its teams
        teams = {t["franchise"]: t for t in json.loads(src.read_text())["teams"]}
        shown = teams  # the whole export was in front of the user: every shade in it is a pick
        teams = {f: t for f, t in teams.items() if t != next((o for o in old["teams"] if o["franchise"] == f), None)}
    else:  # the page's saved documents
        teams = shown = {}
        for f in sorted(src.glob("*.json")):
            doc = json.loads(f.read_text())
            t = doc.get("team", doc)
            teams[t["franchise"]] = t
    index = {t["franchise"]: i for i, t in enumerate(new["teams"])}
    for f, t in teams.items():
        if f not in index:
            raise SystemExit(f"{f}: not a team in {pack_path}")
        base = new["teams"][index[f]]
        # Slot by slot, so an edit that leaves a slot out never drops that player.
        players = {p["slot"]: p for p in base.get("players", [])}
        players.update({p["slot"]: p for p in t.get("players", [])})
        team = {"franchise": f}
        for k in ("city", "nickname", "select", "abbr"):
            v = t[k] if k in t else base.get(k)  # left out: kept; given as "": cleared
            if v:
                team[k] = v
        order = list(base.get("ratings", {})) or list(t.get("ratings", {}))
        team["ratings"] = {k: t.get("ratings", base.get("ratings", {})).get(k, base.get("ratings", {}).get(k, 0)) for k in order}
        team["players"] = [players[k] for k in sorted(players)]
        new["teams"][index[f]] = team
    new["version"] = a.version or datetime.date.today().strftime("%Y.%m.%d")
    packtool.validate_v2(new)
    lines = changelog(old, new)
    for line in lines:
        print(line)
    dump(new, pack_path)
    print(f"{pack_path}: {len(teams)} teams edited, {len(lines)} changes, version {new['version']}")
    if not a.no_espn:
        remember_picks(shown, a.packdir)


def cmd_diff(a):
    for line in changelog(load(a.old), load(a.new)):
        print(line)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("raw")
    b.add_argument("packdir")
    b.add_argument("--version")
    b.add_argument("--no-checks", action="store_true")
    s = sub.add_parser("season")
    s.add_argument("schedule")
    s.add_argument("packdir")
    e = sub.add_parser("edits")
    e.add_argument("edits")
    e.add_argument("packdir")
    e.add_argument("--version")
    e.add_argument("--no-espn", action="store_true", help="don't look the picked shades up on ESPN")
    d = sub.add_parser("diff")
    d.add_argument("old")
    d.add_argument("new")
    a = ap.parse_args(argv)
    {"build": cmd_build, "season": cmd_season, "diff": cmd_diff, "edits": cmd_edits}[a.cmd](a)


if __name__ == "__main__":
    main()
