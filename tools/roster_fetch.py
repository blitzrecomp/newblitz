#!/usr/bin/env python3
"""Roster update pipeline: current NFL starters from ESPN's public JSON into the 2026 pack.

  roster_fetch.py [--pack packs/blitz2026] [--season 2026] [--out build/rosters/espn_raw.json] [--apply]

For each team it reads ESPN's roster (jersey numbers, injury status), depth chart (starters in
order) and who started the last three games (a starter is listed first on the depth chart or started
two of the last three), then fills the game's 15 player slots with healthy starters:
  0-2  offensive line   left tackle, center, right tackle (then guards)
  3    quarterback      4  running back      5-6  receivers (WR1, WR2; a tight end if short)
  7-9  defensive line   ends and tackles (3-4 teams: nose tackle and outside linebackers)
  10-13 defenders       linebackers, cornerbacks, safeties
  14   kicker
A player on a reserve list (injured reserve, PUP, suspension) is skipped for the next on the depth
chart; one who is only out for this week's game keeps his slot. A slot whose current player is still a healthy starter in that group is left alone, so
the pack only changes where the team did. Ratings are kept.

Skin shades picked by hand (packs/blitz2026/picks.json, by ESPN id; roster_pack.py edits writes it) follow
the player to his new slot or team. Each slot also gets "heavy" (the heavier body model) from ESPN's position and weight, as the original
game assigns it: linemen, linebackers and tight ends, plus backs over 225 lb, receivers over 230 and
DBs over 225; never quarterbacks or kickers.

Without --apply it writes the research file and prints what would change; --apply then runs
tools/roster_pack.py build (changelog, validation, version stamp). ESPN's site API is unofficial and
unauthenticated: fine for refreshing a personal pack once a season, not for anything heavier.
"""
import argparse
import json
import subprocess
import sys
import time
import unicodedata
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SITE = "https://site.api.espn.com/apis/site/v2/sports/football/nfl"
CORE = "https://sports.core.api.espn.com/v2/sports/football/leagues/nfl"
ESPN_TO_FRANCHISE = {"WSH": "WAS"}
# Long-term absences only. "Out" and "Doubtful" on ESPN's injury report are about this week's game:
# the season's roster keeps those starters (a week-to-week injury shouldn't bench Lamar Jackson).
OUT_STATUSES = {"injured reserve", "ir", "physically unable to perform", "pup", "suspension", "suspended",
                "non-football injury", "reserve/covid-19", "out for season"}

# Slot groups: depth chart position keys in priority order.
GROUPS = {
    "ol": ["lt", "c", "rt", "lg", "rg"],
    "qb": ["qb"],
    "rb": ["rb"],
    "rec": ["wr", "te"],
    "dl": ["lde", "rde", "ldt", "rdt", "nt", "lolb", "rolb"],
    "def": ["mlb", "lilb", "rilb", "lcb", "rcb", "ss", "fs", "wlb", "slb", "nb"],
    "k": ["pk"],
}
SLOTS = [("ol", 0), ("ol", 1), ("ol", 2), ("qb", 3), ("rb", 4), ("rec", 5), ("rec", 6),
         ("dl", 7), ("dl", 8), ("dl", 9), ("def", 10), ("def", 11), ("def", 12), ("def", 13), ("k", 14)]


def get(url, tries=3):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "BlitzRecomp roster refresh"}), timeout=20) as r:
                return json.load(r)
        except Exception:
            if i == tries - 1:
                raise
            time.sleep(1.5)


def clean_last(name):
    name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()
    words = [w for w in name.replace(",", " ").split() if w.rstrip(".").upper() not in ("JR", "SR", "II", "III", "IV", "V")]
    return " ".join(words)[:16]


def team_ids():
    data = get(f"{SITE}/teams")
    out = {}
    for t in data["sports"][0]["leagues"][0]["teams"]:
        t = t["team"]
        out[ESPN_TO_FRANCHISE.get(t["abbreviation"], t["abbreviation"])] = t["id"]
    return out


def roster(team_id):
    data = get(f"{SITE}/teams/{team_id}/roster")
    players = {}
    for group in data.get("athletes", []):
        for a in group.get("items", []):
            statuses = {(i.get("status") or "").lower() for i in a.get("injuries", [])}
            status = a.get("status") or {}
            statuses.add((status.get("name") or "").lower())
            if (status.get("type") or "active").lower() != "active":
                statuses.add("ir")  # any reserve list
            players[a["id"]] = {
                "id": a["id"], "number": int(a["jersey"]) if str(a.get("jersey", "")).isdigit() else None,
                "last": clean_last(a.get("lastName", "")), "out": bool(statuses & OUT_STATUSES),
                "position": (a.get("position") or {}).get("abbreviation", ""),
                "weight": float(a.get("weight") or 0),
            }
    return players


def depth(team_id, season):
    data = get(f"{CORE}/seasons/{season}/teams/{team_id}/depthcharts")
    chart = {}  # position key -> list of (position slot, [athlete ids in depth order])
    for formation in data.get("items", []):
        for key, pos in formation.get("positions", {}).items():
            by_slot = {}
            for a in pos.get("athletes", []):
                aid = a["athlete"]["$ref"].split("athletes/")[1].split("?")[0]
                by_slot.setdefault(a.get("slot", 0), []).append(aid)
            chart.setdefault(key, [])
            for s in sorted(by_slot):
                if by_slot[s] not in chart[key]:
                    chart[key].append(by_slot[s])
    return chart


def groups_for(chart):
    """3-4 fronts (a nose tackle and inside linebackers) list their edge rushers as outside
    linebackers (wlb/slb); in Blitz those are defensive linemen."""
    if "nt" in chart and ("lilb" in chart or "rilb" in chart):
        return dict(GROUPS, dl=["wlb", "slb", "lde", "rde", "nt", "lolb", "rolb"],
                    **{"def": ["lilb", "rilb", "mlb", "lcb", "rcb", "ss", "fs", "nb"]})
    return GROUPS


# Roster positions of players who started recent games, by slot group (for players the depth chart
# doesn't list first, e.g. a backup who has started since an injury).
POSITIONS = {"ol": {"OT", "T", "G", "OG", "C", "OL"}, "qb": {"QB"}, "rb": {"RB", "HB"}, "rec": {"WR", "TE"},
             "dl": {"DE", "DT", "NT", "DL", "EDGE", "OLB"}, "def": {"LB", "ILB", "MLB", "OLB", "CB", "S", "FS", "SS", "DB"},
             "k": {"PK", "K"}}


def recent_starters(team_id, season, games=3, need=2):
    """Athlete ids who started at least `need` of the team's last `games` completed games."""
    sched = get(f"{SITE}/teams/{team_id}/schedule?season={season}")
    done = [e["id"] for e in sched.get("events", []) if e["competitions"][0]["status"]["type"].get("completed")]
    counts = {}
    for event in done[-games:]:
        try:
            r = get(f"{CORE}/events/{event}/competitions/{event}/competitors/{team_id}/roster")
        except Exception:
            continue
        for entry in r.get("entries", []):
            if entry.get("starter") and not entry.get("didNotPlay"):
                aid = str(entry.get("playerId"))
                counts[aid] = counts.get(aid, 0) + 1
    return {aid for aid, n in counts.items() if n >= min(need, len(done[-games:]))}


def starters(chart, players, group, recent=frozenset()):
    """Healthy starters for a group, in priority order (the first healthy player at each spot)."""
    chart_starters = []
    for key in groups_for(chart)[group]:
        for spot in chart.get(key, []):
            for aid in spot:
                p = players.get(aid)
                if p and not p["out"] and p["number"] is not None:
                    if p not in chart_starters:
                        chart_starters.append(p)
                    break
    played = [p for p in players.values() if p["id"] in recent and not p["out"] and p["number"] is not None
              and p["position"] in POSITIONS[group]]
    # Depth-chart starters who also started recently, then other recent starters, then the rest of the chart.
    both = [p for p in chart_starters if p["id"] in recent]
    return both + [p for p in played if p not in both] + [p for p in chart_starters if p not in both]


def pick_team(team, chart, players, recent=frozenset()):
    current = {p["slot"]: p for p in team.get("players", [])}
    pools = {g: starters(chart, players, g, recent) for g in GROUPS}
    used, picks = set(), {}
    # Keep current players who are still healthy starters in their slot's group. Defensive slots
    # (DL 7-9, defenders 10-13) accept any healthy starting defender: an edge rusher the pack has as
    # a linebacker plays fine there.
    for group, slot in SLOTS:
        cur = current.get(slot, {})
        pool = pools["dl"] + pools["def"] if group in ("dl", "def") else pools[group]
        for p in pool:
            same_name = not cur.get("last") or clean_last(cur["last"]).lower() == p["last"].lower()
            if p["number"] == cur.get("number") and p["id"] not in used and same_name:
                picks[slot] = dict(p, last=cur.get("last", p["last"]))  # kept: the pack's spelling stays
                used.add(p["id"])
                break
    for group, slot in SLOTS:
        if slot in picks:
            continue
        for p in pools[group]:
            if p["id"] not in used:
                picks[slot] = p
                used.add(p["id"])
                break
    return picks


# The heavier body model, as the original game hands it out (its 31 teams: every lineman, linebacker
# and tight end; power backs; no quarterbacks, kickers or receivers, and one DB): by ESPN position,
# and by weight for backs, receivers and DBs.
HEAVY_POSITIONS = {"TE", "LB", "OLB", "ILB", "MLB", "LILB", "RILB", "WILL", "SAM", "MIKE", "DE", "DT", "NT", "EDGE", "DL"}
HEAVY_WEIGHT = {"RB": 225, "FB": 225, "HB": 225, "WR": 230, "CB": 225, "S": 225, "SS": 225, "FS": 225, "DB": 225}


def heavy_for(slot, p):
    if slot in (0, 1, 2, 7, 8, 9):
        return 1
    if slot in (3, 14):
        return 0
    pos = p.get("position", "")
    if pos in HEAVY_POSITIONS:
        return 1
    return 1 if p.get("weight", 0) >= HEAVY_WEIGHT.get(pos, 999) else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", default=str(ROOT / "packs" / "blitz2026"))
    ap.add_argument("--season", default="2026")
    ap.add_argument("--out", default=str(ROOT / "build" / "rosters" / "espn_raw.json"))
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args(argv)
    pack = json.loads((Path(a.pack) / "pack.json").read_text())
    ids = team_ids()
    raw = {"source": f"ESPN site/core JSON, season {a.season}, fetched {time.strftime('%Y-%m-%d')}", "teams": []}
    changes, gaps = [], []
    for team in pack["teams"]:
        f = team["franchise"]
        if f not in ids:
            gaps.append(f"{f}: not in ESPN's team list")
            continue
        players, chart = roster(ids[f]), depth(ids[f], a.season)
        picks = pick_team(team, chart, players, recent_starters(ids[f], a.season))
        cur = {p["slot"]: p for p in team.get("players", [])}
        out_players = []
        for _, slot in SLOTS:
            p = picks.get(slot)
            if p is None:
                gaps.append(f"{f} slot {slot}: no healthy starter found; kept")
                if slot in cur:
                    out_players.append({k: v for k, v in cur[slot].items() if k in ("slot", "number", "last", "heavy")})
                continue
            out_players.append({"slot": slot, "number": p["number"], "last": p["last"], "heavy": heavy_for(slot, p),
                                "espn": p["id"]})  # for picks.json (roster_pack.py build); not written to the pack
            old = cur.get(slot, {})
            if (old.get("number"), clean_last(old.get("last", "")).lower()) != (p["number"], clean_last(p["last"]).lower()) and \
                    not (old.get("number") == p["number"] and not old.get("last")):
                was = next((q for q in players.values() if q["number"] == old.get("number")), None)
                why = ("not on the roster" if was is None else "on a reserve list" if was["out"]
                       else f"not a starter ({was['position']})")
                changes.append(f"{f} slot {slot:2}: {old.get('last', '?')} #{old.get('number', '?')} -> {p['last']} #{p['number']} ({p['position']}); old: {why}")
        raw["teams"].append({"franchise": f, "ratings": team["ratings"], "players": out_players})
        time.sleep(0.2)
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text(json.dumps(raw, indent=1))
    print("\n".join(changes) or "no changes")
    print("\n".join(gaps))
    print(f"{len(changes)} slot changes; research written to {a.out}")
    if a.apply:
        subprocess.run([sys.executable, str(ROOT / "tools" / "roster_pack.py"), "build", a.out, a.pack, "--no-checks"], check=True)


if __name__ == "__main__":
    main()
