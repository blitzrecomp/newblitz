#!/usr/bin/env python3
"""Real college rosters for a league pak, for personal use: fills each team's 15 slots with this season's
starters from ESPN's public college-football JSON, and writes ONLY to a pak in an app folder.

  .venv/bin/python tools/college_roster.py --pack <app folder>/packs/pac12 [--games 4] [--season 2026] [--dry-run]

College players' names belong to them; the pak in this repo (packs/pac12) keeps its fictional players, and
this script refuses to write anywhere in the repo except build/. ESPN has no college depth charts, so a
starter is someone who started recent games (each box score lists the 22 starters):
  0-2  offensive line   the three linemen with the most recent starts (280 lb+ starters if ESPN lists none)
  3    quarterback      the most recent starter (passing yards break a tie)
  4    running back     most rushing yards among recent starters, else on the roster
  5-6  receivers        most receiving yards (wide receivers, then a tight end)
  7-9  defensive line   starting linemen, by tackles
  10-13 defenders       starting linebackers and backs, by tackles
  14   kicker           the roster's kicker
Players who aren't active (injured reserve, out, suspended) are skipped. Ratings, skin and art are untouched.
Each team in the pak needs its ESPN team id: the Pac-12 schools are built in; --espn ID=espn_id adds others.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import roster_fetch as rf  # noqa: E402  (get, clean_last, heavy_for)

SITE = "https://site.api.espn.com/apis/site/v2/sports/football/college-football"
CORE = "https://sports.core.api.espn.com/v2/sports/football/leagues/college-football"
ESPN_IDS = {"BSU": 68, "CSU": 36, "FRE": 278, "ORS": 204, "SDS": 21, "TXS": 326, "USU": 328, "WSU": 265}
GROUPS = {"ol": {"OL", "OT", "OG", "G", "T", "C", "IOL"}, "qb": {"QB"}, "rb": {"RB", "FB", "HB"}, "wr": {"WR"}, "te": {"TE"},
          "dl": {"DL", "DE", "DT", "NT", "EDGE"}, "def": {"LB", "ILB", "OLB", "MLB", "DB", "CB", "S", "FS", "SS"}, "k": {"PK", "K"}}


def roster(team_id):
    data = rf.get(f"{SITE}/teams/{team_id}/roster")
    players = {}
    for group in data.get("athletes", []):
        inactive = group.get("position") in ("injuredReserveOrOut", "suspended")
        for a in group.get("items", []):
            status = (a.get("status") or {}).get("type", "active").lower()
            jersey = str(a.get("jersey", ""))
            players[str(a["id"])] = {
                "id": str(a["id"]), "last": rf.clean_last(a.get("lastName", "")),
                "number": int(jersey) if jersey.isdigit() and int(jersey) <= 99 else None,
                "position": (a.get("position") or {}).get("abbreviation", ""), "weight": float(a.get("weight") or 0),
                "out": inactive or status != "active" or bool(a.get("injuries")),
                "starts": 0, "pass": 0, "rush": 0, "rec": 0, "tackles": 0,
            }
    return players


def recent_games(team_id, season, games, players):
    sched = rf.get(f"{SITE}/teams/{team_id}/schedule?season={season}")
    done = [e["id"] for e in sched.get("events", []) if e["competitions"][0]["status"]["type"].get("completed")][-games:]
    for event in done:
        try:
            r = rf.get(f"{CORE}/events/{event}/competitions/{event}/competitors/{team_id}/roster")
            for entry in r.get("entries", []):
                p = players.get(str(entry.get("playerId")))
                if p and entry.get("starter") and not entry.get("didNotPlay"):
                    p["starts"] += 1
            box = rf.get(f"{SITE}/summary?event={event}").get("boxscore", {}).get("players", [])
        except Exception as e:  # one missing game shouldn't stop the team
            print(f"  game {event}: {e}", file=sys.stderr)
            continue
        for side in box:
            if str(side["team"]["id"]) != str(team_id):
                continue
            for stat in side["statistics"]:
                key = {"passing": "pass", "rushing": "rush", "receiving": "rec", "defensive": "tackles"}.get(stat["name"])
                if not key:
                    continue
                col = 1 if key != "tackles" else 0  # YDS, or TOT tackles
                for a in stat.get("athletes", []):
                    p = players.get(str(a["athlete"].get("id")))
                    if p is None:
                        continue  # not on today's roster
                    try:
                        p[key] += float(str(a["stats"][col]).split("/")[0])
                    except (ValueError, IndexError):
                        pass
    return len(done)


def pick(players):
    ok = [p for p in players.values() if not p["out"] and p["number"] is not None and p["last"]]
    used = set()

    def take(group, n, key, starters_only=True):
        pool = [p for p in ok if p["position"] in GROUPS[group] and p["id"] not in used and (p["starts"] or not starters_only)]
        pool.sort(key=lambda p: (-p["starts"], -p[key], -p["weight"]) if key == "starts" else (-p[key], -p["starts"]))
        got = pool[:n]
        used.update(p["id"] for p in got)
        return got

    def fill(group, n, key):
        got = take(group, n, key)
        return got + take(group, n - len(got), key, starters_only=False) if len(got) < n else got

    slots = {}
    ol = fill("ol", 3, "starts")
    if len(ol) < 3:  # ESPN sometimes lists a school's linemen under another position: a 280 lb+ starter is one
        big = sorted((p for p in ok if p["id"] not in used and p["starts"] and p["weight"] >= 280), key=lambda p: (-p["starts"], -p["weight"]))
        ol += big[:3 - len(ol)]
        used.update(p["id"] for p in ol)
    ol.sort(key=lambda p: -p["weight"])  # tackles are usually the biggest: the heaviest two outside, the center between
    for slot, p in zip((0, 2, 1), ol):
        slots[slot] = p
    for slot, p in zip((3,), fill("qb", 1, "starts")):
        slots[slot] = p
    for slot, p in zip((4,), fill("rb", 1, "rush")):
        slots[slot] = p
    rec = fill("wr", 2, "rec")
    if len(rec) < 2:
        rec += fill("te", 2 - len(rec), "rec")
    for slot, p in zip((5, 6), rec):
        slots[slot] = p
    for slot, p in zip((7, 8, 9), fill("dl", 3, "tackles")):
        slots[slot] = p
    for slot, p in zip((10, 11, 12, 13), fill("def", 4, "tackles")):
        slots[slot] = p
    for slot, p in zip((14,), fill("k", 1, "starts")):
        slots[slot] = p
    return slots


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pack", required=True, help="the league pak folder in an app folder (its pack.json is updated)")
    ap.add_argument("--season", type=int, default=2026)
    ap.add_argument("--games", type=int, default=4, help="recent games that count as starts")
    ap.add_argument("--espn", action="append", default=[], metavar="ID=ESPN_ID", help="ESPN team id for a team not built in")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    pack_dir = Path(a.pack).expanduser().resolve()
    build = (ROOT / "build").resolve()
    if (pack_dir == ROOT or ROOT in pack_dir.parents) and build not in pack_dir.parents:
        raise SystemExit("Real college rosters stay out of the repo: point --pack at an app folder's packs/<id> "
                         "(e.g. build/appdata2026/packs/pac12 or ~/Library/Application Support/.../packs/pac12).")
    path = pack_dir / "pack.json"
    pack = json.loads(path.read_text())
    if "league" not in pack:
        raise SystemExit(f"{path} isn't a league pak")
    ids = dict(ESPN_IDS, **{k: int(v) for k, v in (x.split("=", 1) for x in a.espn)})
    for team in pack["teams"]:
        tid = team["id"]
        if tid not in ids:
            print(f"{tid}: no ESPN id (use --espn {tid}=...), kept")
            continue
        players = roster(ids[tid])
        n = recent_games(ids[tid], a.season, a.games, players)
        slots = pick(players)
        old = {p["slot"]: p for p in team.get("players", [])}
        new = []
        for slot in range(15):
            p = slots.get(slot)
            if not p:
                if slot in old:
                    new.append(old[slot])
                continue
            entry = {"slot": slot, "last": p["last"][:15], "number": p["number"], "heavy": rf.heavy_for(slot, p)}
            if "skin" in old.get(slot, {}) and old[slot].get("last") == entry["last"]:
                entry["skin"] = old[slot]["skin"]  # a shade you set for the same player stays
            new.append(entry)
        team["players"] = new
        qb = slots.get(3)
        print(f"{tid}: {len(slots)}/15 slots from {n} recent games; QB #{qb['number']} {qb['last']}" if qb else f"{tid}: {len(slots)}/15 slots")
    if a.dry_run:
        print(json.dumps([{t["id"]: [(p["slot"], p.get("number"), p.get("last")) for p in t["players"]]} for t in pack["teams"]], indent=0))
        return
    path.write_text(json.dumps(pack, indent=1) + "\n")
    print(f"wrote {path} (personal use; restart the game to see it)")


if __name__ == "__main__":
    main()
