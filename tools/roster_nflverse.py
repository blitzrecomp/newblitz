#!/usr/bin/env python3
"""The roster job's open-data source (2026-10-26): nflverse's daily files instead of ESPN's API.

roster_season.py `--source nflverse` (the default) reads the league from here; `--source espn` keeps the ESPN flow
(roster_fetch.py) as it was. Same shapes as ESPN's side, so the lineup picking, player file and manifest don't change:

  fetch_league(season, with_recent)  {franchise: (roster {id: player}, depth chart, recent starters, depth spots)}
  current_week()                     (season, "2026-w07" / "2026-post1" / "2026-pre", kind)
  kickoffs(season, label)            {franchise: kickoff (UTC)} for that week's games
  transactions_since(since)          [] (nflverse has no transaction notes)

From nflverse-data's GitHub releases (https://github.com/nflverse/nflverse-data, CC-BY-4.0; the facts come from the
league's own feeds), refreshed every morning around 06:00 UTC:
  rosters/roster_<season>.csv          every player's club, number, position, weight and status (ACT, INA, DEV, RES)
  depth_charts/depth_charts_<season>.csv  the clubs' depth charts twice a day, by ESPN id and ESPN's position keys
  injuries/injuries_<season>.csv       the weekly injury report (Out, Doubtful ...)
  snap_counts/snap_counts_<season>.csv who played most of each game (stands in for ESPN's game starters)
  schedules/games.csv                  weeks, kickoffs, results

Players stay keyed by ESPN id (the player file, crews and the Roster Room's headshots all use it): the depth charts
carry both ids, the roster file most; anyone with neither is matched by name and club, else keyed "nfl:<gsis id>".
"""
import csv
import datetime
import io
import sys
import urllib.request
import zoneinfo
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from roster_fetch import clean_last  # noqa: E402

BASE = "https://github.com/nflverse/nflverse-data/releases/download"
TEAM = {"LA": "LAR"}  # nflverse -> our franchise codes (the rest agree)
# nflverse depth-chart positions -> the ESPN abbreviations the rest of the job (and the player file) uses.
POSITION = {"T": "OT", "K": "PK", "OLB": "LB", "ILB": "LB", "MLB": "LB", "FS": "S", "SS": "S"}
ON_CLUB = {"ACT", "INA", "DEV", "RES", "EXE"}  # INA: active, inactive for a game; DEV: practice squad; RES: reserve
RESERVE = {"RES", "EXE"}                       # lists; EXE: exempt (on the club, can't play)
WEEK_OUT = {"out", "doubtful"}
EASTERN = zoneinfo.ZoneInfo("America/New_York")
_cache = {}


def get_csv(release, name):
    """A release file's rows (cached for the run)."""
    key = f"{release}/{name}"
    if key not in _cache:
        for attempt in range(3):
            try:
                req = urllib.request.Request(f"{BASE}/{key}", headers={"User-Agent": "BlitzRecomp roster job"})
                with urllib.request.urlopen(req, timeout=120) as r:
                    _cache[key] = list(csv.DictReader(io.StringIO(r.read().decode("utf-8"))))
                break
            except Exception:
                if attempt == 2:
                    raise
    return _cache[key]


def sleeper_espn_ids():
    """{gsis id: ESPN id} from Sleeper's public player list (free, no key; https://docs.sleeper.com): only to fill
    the ESPN ids nflverse lacks (about a third of its roster rows). Empty if Sleeper can't be reached."""
    import json
    try:
        req = urllib.request.Request("https://api.sleeper.app/v1/players/nfl", headers={"User-Agent": "BlitzRecomp roster job"})
        with urllib.request.urlopen(req, timeout=120) as r:
            players = json.loads(r.read())
    except Exception as e:
        print(f"[nflverse] Sleeper's player list: {e}; going on without it")
        return {}
    return {str(p.get("gsis_id") or "").strip(): str(p["espn_id"]) for p in players.values()
            if p.get("gsis_id") and p.get("espn_id")}


def team(code):
    return TEAM.get(code, code)


def _int(s):
    try:
        return int(float(s))
    except (TypeError, ValueError):
        return None


# --- The week ---------------------------------------------------------------------------------------------------------

def games(season):
    return [g for g in get_csv("schedules", "games.csv") if g["season"] == str(season)]


POST = {"WC": 1, "DIV": 2, "CON": 3, "SB": 4}


def label_of(g):
    year = g["season"]
    if g["game_type"] == "REG":
        return f"{year}-w{int(g['week']):02}"
    return f"{year}-post{POST.get(g['game_type'], 1)}"


def kickoff(g):
    """A game's kickoff in UTC (the schedule's times are Eastern)."""
    t = datetime.datetime.strptime(f"{g['gameday']} {g['gametime'] or '13:00'}", "%Y-%m-%d %H:%M")
    return t.replace(tzinfo=EASTERN).astimezone(datetime.timezone.utc)


def current_week():
    """The season and the week whose games aren't all played yet (ESPN's scoreboard turns over the same way)."""
    now = datetime.datetime.now(datetime.timezone.utc)
    season = now.year if now.month >= 3 else now.year - 1
    gs = games(season)
    if not gs:
        return season, f"{season}-pre", "pre"
    open_games = [g for g in gs if g["result"] == ""]
    if not open_games:
        return season, label_of(gs[-1]), "week"
    first = min(open_games, key=kickoff)
    if first["game_type"] == "REG" and int(first["week"]) == 1 and all(g["result"] == "" for g in gs):
        if now < kickoff(first) - datetime.timedelta(days=7):
            return season, f"{season}-pre", "pre"
    return season, label_of(first), "week"


def kickoffs(season, label):
    """{franchise: kickoff} for a week's games (a team on bye has none)."""
    out = {}
    for g in games(season):
        if label_of(g) == label:
            for side in ("home_team", "away_team"):
                out[team(g[side])] = kickoff(g)
    return out


def transactions_since(since):
    return []


# --- The league -------------------------------------------------------------------------------------------------------

def depth_rows(season):
    """Each club's newest depth chart (the file keeps every snapshot)."""
    rows = get_csv("depth_charts", f"depth_charts_{season}.csv")
    newest = {}
    for r in rows:
        f = team(r["team"])
        if r["dt"] > newest.get(f, ""):
            newest[f] = r["dt"]
    return [r for r in rows if r["dt"] == newest[team(r["team"])]]


def recent_starters(season, by_pfr, games_back=3, need=2, share=0.6):
    """{franchise: ESPN ids} who played most of the snaps (offense or defense) in `need` of the club's last
    `games_back` games: ESPN's "started" stood in for by snap counts."""
    rows = get_csv("snap_counts", f"snap_counts_{season}.csv")
    weeks = {}
    for r in rows:
        weeks.setdefault(team(r["team"]), set()).add(int(r["week"]))
    counts = {}
    for r in rows:
        f = team(r["team"])
        if int(r["week"]) not in sorted(weeks[f])[-games_back:]:
            continue
        pct = max(float(r["offense_pct"] or 0), float(r["defense_pct"] or 0))
        pid = by_pfr.get(r["pfr_player_id"])
        if pid and pct >= share:
            counts.setdefault(f, {}).setdefault(pid, 0)
            counts[f][pid] += 1
    return {f: {pid for pid, n in c.items() if n >= min(need, len(sorted(weeks[f])[-games_back:]))}
            for f, c in counts.items()}


def fetch_league(season, with_recent=True, known=None):
    """The 32 clubs as roster_season.fetch_league builds them from ESPN. `known` (the player file) lets a player
    without an ESPN id keep the key he already has, by name and club."""
    depth = depth_rows(season)
    gsis_to_espn = sleeper_espn_ids()
    gsis_to_espn.update({r["gsis_id"]: r["espn_id"] for r in depth if r["gsis_id"] and r["espn_id"]})
    norm = lambda s: "".join(c for c in clean_last(s).lower() if c.isalpha())
    by_name, name_count = {}, {}
    for pid, e in (known or {}).items():
        n = norm(f"{e.get('first', '')} {e.get('last', '')}")
        name_count[n] = name_count.get(n, 0) + 1
        by_name[(n, e.get("team"))] = pid
        by_name[(n, "*")] = pid

    def known_id(first, last, f):
        """The player file's key for a name: on the same club, else the only player with that name anywhere."""
        n = norm(f"{first} {last}")
        return by_name.get((n, f)) or (by_name.get((n, "*")) if name_count.get(n) == 1 else None)

    # One row per player, his status now (its week is when it last changed): with a club, or not (CUT, RET ...).
    rows = get_csv("rosters", f"roster_{season}.csv")
    league_rows = [r for r in rows if r["status"] in ON_CLUB]

    # This week's injury report, by gsis id.
    _, label, _ = current_week()
    inj_week = None
    if "-w" in label:
        inj_week = str(int(label.split("-w")[1]))
    injuries = [r for r in get_csv("injuries", f"injuries_{season}.csv") if r["season_type"] == "REG"]
    out_week = {r["gsis_id"] for r in injuries if r["week"] == inj_week and r["report_status"].lower() in WEEK_OUT}
    # On any of the last three weeks' injury reports (a game status, or limited or out of practice): working back from
    # an injury. Full strength keeps such a starter in his slot (Terry McLaurin, 2026 weeks 4-6: a hamstring; the depth
    # chart had him WR6).
    weeks = sorted({int(r["week"]) for r in injuries})
    recent_weeks = {str(w) for w in weeks[-3:]}
    hurt = {r["gsis_id"] for r in injuries if r["week"] in recent_weeks and
            (r["report_status"] or "limited" in r["practice_status"].lower() or "did not" in r["practice_status"].lower())}

    rosters, by_pfr = {}, {}
    for r in league_rows:
        f = team(r["team"])
        pid = r["espn_id"] or gsis_to_espn.get(r["gsis_id"])
        if f"nfl:{r['gsis_id']}" in (known or {}):  # keyed before his ESPN id was known: keeps that key
            pid = f"nfl:{r['gsis_id']}"
        first = r["football_name"] or r["first_name"]
        if not pid:
            pid = known_id(first, r["last_name"], f) or f"nfl:{r['gsis_id']}"
        reserve = r["status"] in RESERVE
        status = "practice" if r["status"] == "DEV" else "reserve" if reserve else \
            "out" if r["gsis_id"] in out_week else "active"
        pos = r["depth_chart_position"] or r["position"]
        rosters.setdefault(f, {})[pid] = {
            "id": pid, "first": first, "last": clean_last(r["last_name"]), "number": _int(r["jersey_number"]),
            "position": POSITION.get(pos, pos), "weight": float(r["weight"] or 0), "status": status,
            "reserve": reserve, "out_week": reserve or r["gsis_id"] in out_week, "hurt_recently": r["gsis_id"] in hurt,
        }
        if r["pfr_id"]:
            by_pfr[r["pfr_id"]] = pid

    # Someone the player file has with a club who isn't in nflverse's file at all (a gap in its data, not a release:
    # a released player stays in the file as CUT) is kept as he was.
    listed = {r["espn_id"] or gsis_to_espn.get(r["gsis_id"], "") for r in rows}
    listed |= {known_id(r["football_name"] or r["first_name"], r["last_name"], team(r["team"])) for r in rows}
    kept = 0
    for pid, e in (known or {}).items():
        if e.get("team") and pid not in listed and not any(pid in ros for ros in rosters.values()):
            rosters.setdefault(e["team"], {})[pid] = {
                "id": pid, "first": e.get("first", ""), "last": e.get("last", ""), "number": e.get("number"),
                "position": e.get("pos", ""), "weight": float(e.get("weight") or 0), "status": e.get("status", "active"),
                "reserve": e.get("status") == "reserve", "out_week": e.get("status") in ("reserve", "out")}
            kept += 1
    if kept:
        print(f"[nflverse] {kept} players missing from nflverse's roster file kept as they were")

    # Depth charts: ESPN's shape (position key -> spots, each a list of ids in rank order), formations merged; and
    # each player's best spot ("wr2"), a return spot only when he has no other.
    charts, ranks = {}, {}
    return_keys = {"kr", "pr", "h", "ls"}
    grouped = {}
    for r in depth:
        if not r["espn_id"]:
            continue
        key = {"k": "pk"}.get(r["pos_abb"].lower(), r["pos_abb"].lower())  # ESPN's key for the kicker
        grouped.setdefault((team(r["team"]), r["pos_grp"], key, int(r["pos_slot"])), []).append((int(r["pos_rank"]), r["espn_id"]))
    for (f, _, key, _), entries in sorted(grouped.items()):
        ids = [pid for _, pid in sorted(entries)]
        chart = charts.setdefault(f, {}).setdefault(key, [])
        if ids not in chart:
            chart.append(ids)
        for rank, pid in sorted(entries):
            cur = ranks.setdefault(f, {}).get(pid)
            better = cur is None or (cur[0] in return_keys and key not in return_keys) or \
                ((cur[0] in return_keys) == (key in return_keys) and rank < cur[1])
            if better:
                ranks[f][pid] = (key, rank)

    recent = recent_starters(season, by_pfr) if with_recent else {}
    league = {}
    for f in sorted(rosters):
        league[f] = (rosters[f], charts.get(f, {}), recent.get(f, set()),
                     {pid: f"{k}{r}" for pid, (k, r) in ranks.get(f, {}).items()})
    return league


if __name__ == "__main__":  # a quick look: python3 tools/roster_nflverse.py
    s, lab, kind = current_week()
    lg = fetch_league(s)
    print(s, lab, kind, len(lg), "clubs,", sum(len(v[0]) for v in lg.values()), "players")
    ko = kickoffs(s, lab)
    print("kickoffs:", {f: t.strftime("%a %H:%MZ") for f, t in sorted(ko.items())})
