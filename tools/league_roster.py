#!/usr/bin/env python3
"""A league pak's pack.json players and ratings from the research handoff's selected_players.json files.

  .venv/bin/python tools/league_roster.py --research <dir>/league_pak_research/pac12 --pack <app folder>/packs/pac12/pack.json
                                          [--teams BSU,CSU] [--modes arcade,tournament] [--version 2026.11.27]

Each <research>/<ID>/selected_players.json wraps every player in research metadata (sources, measurements, confidence,
appearance audit); only its "blitz" object goes into the pak, and only the keys the game accepts
(src/game/content_pack_tp.cpp parse_players: slot, expect, last, number, size, skin, heavy, announcer_call). The research
stays in the handoff. Team ids, bases and names are the league config's (tools/leagues/<name>.json, --league); ratings
come from the research file.

Checks the game's own rules first (slot 0-15 once each, slot 3 named, names of 1-15 drawable characters, number 0-99,
skin 0-5, heavy 0/1) and stops on any break: nothing is renamed or renumbered. Duplicate jersey numbers in a team are
legal (the parser doesn't check them) and kept; each is listed, and one the research file doesn't document stops the run.

An existing pack.json keeps its art keys (uniform_files, uniform_sets, vs_helmet_right, clashes_with) per team.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from make_league_pak import load_league  # noqa: E402

PLAYER_KEYS = {"slot", "expect", "last", "number", "size", "skin", "heavy", "announcer_call"}
RATINGS = ["passing", "rushing", "linemen", "defense", "special_teams"]
ART_KEYS = ["uniform_files", "uniform_sets", "vs_helmet_right", "clashes_with", "uv_override", "cart_files"]


def drawable(s, name):
    return all(c.isascii() and (c.isalpha() or (not name and c.isdigit()) or c in " .'-") for c in s)


def runtime_player(p, where):
    """The legal runtime object of one research player, or SystemExit on anything the game would reject."""
    b = p.get("blitz")
    if not isinstance(b, dict):
        raise SystemExit(f"{where}: no \"blitz\" object")
    extra = set(b) - PLAYER_KEYS
    if extra:
        raise SystemExit(f"{where}: keys the game rejects in \"blitz\": {sorted(extra)}")
    out = {k: b[k] for k in ("slot", "expect", "last", "number", "size", "skin", "heavy", "announcer_call") if k in b}
    slot = out.get("slot")
    if not isinstance(slot, int) or not 0 <= slot <= 15:
        raise SystemExit(f"{where}: slot {slot!r} isn't 0-15")
    last = out.get("last")
    if last is not None and (not isinstance(last, str) or not 1 <= len(last) <= 15 or not drawable(last, True)):
        raise SystemExit(f"{where}: name {last!r} isn't 1-15 letters, space, . ' -")
    if "number" in out and (not isinstance(out["number"], int) or not 0 <= out["number"] <= 99):
        raise SystemExit(f"{where}: number {out['number']!r} isn't 0-99")
    if "skin" in out and out["skin"] not in range(6):
        raise SystemExit(f"{where}: skin {out['skin']!r} isn't 0-5")
    if "heavy" in out and out["heavy"] not in (0, 1):
        raise SystemExit(f"{where}: heavy {out['heavy']!r} isn't 0 or 1")
    if "size" in out and not (isinstance(out["size"], (int, float)) and 0.9 <= out["size"] <= 1.3):
        raise SystemExit(f"{where}: size {out['size']!r} isn't 0.90-1.30")
    if out.get("announcer_call", "none") not in ("none", "keep"):
        raise SystemExit(f"{where}: announcer_call {out['announcer_call']!r}")
    return out


def team_players(path, tid):
    d = json.loads(path.read_text())
    if d.get("team_id", tid) != tid:
        raise SystemExit(f"{path}: team_id {d.get('team_id')} isn't {tid}")
    players = [runtime_player(p, f"{tid} {p.get('full_name', '?')}") for p in d["players"]]
    slots = [p["slot"] for p in players]
    if len(set(slots)) != len(slots):
        raise SystemExit(f"{tid}: a slot appears twice ({sorted(slots)})")
    if not any(p["slot"] == 3 and p.get("last") for p in players):
        raise SystemExit(f"{tid}: slot 3 (the quarterback at team select) has no name")
    by_number = {}
    for p in players:
        if "number" in p:
            by_number.setdefault(p["number"], []).append(p["last"])
    dups = {n: names for n, names in by_number.items() if len(names) > 1}
    documented = {int(k): v for k, v in (d.get("duplicate_numbers") or {}).items()}
    for n, names in dups.items():
        if n not in documented:
            raise SystemExit(f"{tid}: number {n} is worn by {names} and selected_players.json doesn't document it")
    ratings = d.get("ratings") or {}
    for k in ratings:
        if k not in RATINGS or ratings[k] not in range(6):
            raise SystemExit(f"{tid}: rating {k}={ratings[k]!r}")
    return sorted(players, key=lambda p: p["slot"]), ratings, dups, d


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--research", required=True, help="the league's research folder: <ID>/selected_players.json")
    ap.add_argument("--pack", required=True, help="pack.json to write (its art keys are kept when it exists)")
    ap.add_argument("--league", default="pac12", help="tools/leagues/<name>.json, or a path to a league config")
    ap.add_argument("--teams", help="comma-separated ids (default: every team in the league config)")
    ap.add_argument("--modes", default="arcade,tournament")
    ap.add_argument("--version", default="2026.11.27")
    a = ap.parse_args(argv)
    research, pack_path = Path(a.research), Path(a.pack)
    want = set(a.teams.split(",")) if a.teams else None
    old = json.loads(pack_path.read_text()) if pack_path.exists() else {}
    old_teams = {t["id"]: t for t in old.get("teams", [])}
    teams, report = [], []
    for ct in load_league(a.league)["teams"]:
        tid, base, city, nick, select, abbr = (ct[k] for k in ("id", "base", "city", "nickname", "select", "abbr"))
        if want and tid not in want:
            continue
        players, ratings, dups, d = team_players(research / tid / "selected_players.json", tid)
        team = {"id": tid, "base": base, "city": city, "nickname": nick, "select": select, "abbr": abbr}
        for k in ART_KEYS:
            if k in old_teams.get(tid, {}):
                team[k] = old_teams[tid][k]
        team["ratings"] = {k: ratings[k] for k in RATINGS if k in ratings}
        team["players"] = players
        teams.append(team)
        qb = next(p for p in players if p["slot"] == 3)
        report.append(f"{tid}: {len(players)} players, QB {qb['last']} #{qb.get('number')}, snapshot {d.get('snapshot')}"
                      + (", duplicate numbers kept: " + "; ".join(f"#{n} {'/'.join(v)}" for n, v in sorted(dups.items())) if dups else ""))
    pack = {
        "format": "blitz-pack/2",
        "id": old.get("id", "pac12"),
        "title": old.get("title", "Blitz: Pac-12"),
        "version": a.version,
        "requires_rom": "blitz2000-teamplus",
        "league": {
            "name": "Pac-12",
            "modes": a.modes.split(","),
            "ladder": old.get("league", {}).get("ladder", [["USU", "TXS"], ["CSU", "SDS"], ["ORS", "FRE"], ["WSU", "BSU"]]),
            "ladder_done": old.get("league", {}).get("ladder_done", ["BSU", "WSU", "FRE", "SDS"]),
        },
        "teams": teams,
        **({"uv_overrides": old["uv_overrides"]} if "uv_overrides" in old else {}),
    }
    pack_path.parent.mkdir(parents=True, exist_ok=True)
    pack_path.write_text(json.dumps(pack, indent=1) + "\n")
    print("\n".join(report))
    print(f"{pack_path}: {len(teams)} teams, modes {pack['league']['modes']}")


if __name__ == "__main__":
    main()
