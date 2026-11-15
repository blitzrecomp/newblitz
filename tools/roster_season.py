#!/usr/bin/env python3
"""Weekly rosters for Modern Blitz (docs/design/ROSTER_UPDATES.md): a player file plus lineup checkpoints.

  roster_season.py load   [--dir D] [--pack packs/blitz2026]   once a year (after cuts): every player, full strength
  roster_season.py week   [--dir D] [--season Y] [--week N]    before the games: this week's checkpoint
  roster_season.py shades EXPORT [--dir D]                     Roster Room picks (pack.json, picks.json, or the
                                                               page's `shades` folder) -> players.json
  roster_season.py new    [--dir D]                            players who need a skin shade, starters first

D holds (default build/rosters; the cloud job runs in the blitz-rosters checkout):
  players.json            every player on the 32 clubs (active, practice squad, reserve lists) and anyone who has
                          left one since (team null: a free agent), keyed by ESPN id: name, number, position,
                          team, weight, heavy, status, depth-chart spot ("wr2"), skin (null: needs a shade;
                          nothing is guessed)
  lineups/full.json       full strength: each team's 15 slots as ESPN ids, as if everyone were healthy
  lineups/2026-w07.json   a week's checkpoint: healthy starters for that week's games
  psp/pack.txt            the PSP's copy (docs/PSP_PORT.md §6): the players the 4 latest weeks and full strength use,
                          as one small text file (tools/psp/make_pack.py, vendored in blitz-rosters as tools/psp_pack.py)
  manifest.json           what the app downloads: the newest checkpoint and every file's hash

Each run reads all 32 rosters and compares them with the player file: someone on a roster who isn't in the file
(an undrafted free agent signed, a veteran back from retirement) is added and flagged for a shade; someone who's
left every roster becomes a free agent. ESPN's transactions feed is text only (no player ids), so it's kept as
notes, not used to match players.

Slots and picking follow roster_fetch.py (the depth chart, recent starts, the injury report). A week's checkpoint
also benches players listed Out or Doubtful for that week's game; full strength ignores injuries, keeping a starter
who's hurt or on a reserve list as long as he's still with the club.

Sources (--source): nflverse (the default since 2026-10-26: open data, tools/roster_nflverse.py; the job runs daily
and each club's slots in a week's checkpoint freeze at its kickoff), or espn (the original flow, kept as the backup:
ESPN's unofficial API, `git tag espn-flow` in both repos).
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from roster_fetch import (CORE, GROUPS, OUT_STATUSES, POSITIONS, SITE, SLOTS, clean_last, get, heavy_for,  # noqa: E402
                          recent_starters, starters, team_ids)

ROOT = Path(__file__).resolve().parent.parent
FORMAT = "blitz-rosters/1"
# Who takes which file (the manifest's "targets"; blitz_platform.h's names). The PSP reads psp/pack.txt only; everything
# else reads players.json and lineups/. ANY NEW PLATFORM MUST BE ADDED TO JSON_PLATFORMS, or it loses players.json and
# the lineups (rosters_live.cpp's wanted() skips a file whose list doesn't name the device).
JSON_PLATFORMS = ["macOS", "Windows", "Linux", "iOS", "Android", "PS5"]
PSP_PLATFORMS = ["PSP"]
PSP_PACK = "psp/pack.txt"
PSP_PACK_WEEKS = 4
WEEK_OUT = {"out", "doubtful"}  # this week's game only: a checkpoint benches them, full strength doesn't
FRANCHISES = ["ARI", "ATL", "BAL", "BUF", "CAR", "CHI", "CIN", "CLE", "DAL", "DEN", "DET", "GB", "HOU", "IND", "JAX", "KC",
              "LAC", "LAR", "LV", "MIA", "MIN", "NE", "NO", "NYG", "NYJ", "PHI", "PIT", "SEA", "SF", "TB", "TEN", "WAS"]


def today():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")


def read_json(path, default=None):
    path = Path(path)
    return json.loads(path.read_text()) if path.exists() else default


def write_json(path, data, stamp=None):
    """`stamp`: a timestamp field that alone doesn't count as a change: a file that differs only there is left as it
    was, so a quiet day changes no hashes and no device downloads anything."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if stamp and path.exists():
        try:
            old = json.loads(path.read_text())
            if {k: v for k, v in old.items() if k != stamp} == {k: v for k, v in data.items() if k != stamp}:
                return
        except ValueError:
            pass
    path.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n")


# --- ESPN ---------------------------------------------------------------------------------------------------------

def club_roster(team_id):
    """Everyone on a club: {id: player}, each with this week's and long-term availability."""
    data = get(f"{SITE}/teams/{team_id}/roster")
    out = {}
    for group in data.get("athletes", []):
        for a in group.get("items", []):
            injuries = {(i.get("status") or "").lower() for i in a.get("injuries", [])}
            status = a.get("status") or {}
            kind = (status.get("type") or "active").lower()
            reserve = kind not in ("active", "practice-squad") or bool((injuries | {(status.get("name") or "").lower()}) & OUT_STATUSES)
            out_week = reserve or bool(injuries & WEEK_OUT)
            jersey = str(a.get("jersey", ""))
            out[a["id"]] = {
                "id": a["id"], "first": a.get("firstName", ""), "last": clean_last(a.get("lastName", "")),
                "number": int(jersey) if jersey.isdigit() else None,
                "position": (a.get("position") or {}).get("abbreviation", ""), "weight": float(a.get("weight") or 0),
                "status": "practice" if kind == "practice-squad" else "reserve" if reserve else "out" if out_week else "active",
                "reserve": reserve, "out_week": out_week,
            }
    return out


def depth(team_id, season):
    """roster_fetch.depth's chart (position key -> spots in depth order, formations merged), plus each player's
    best spot as ESPN ranks it, a return spot only if he has no other: {id: "wr2"}."""
    data = get(f"{CORE}/seasons/{season}/teams/{team_id}/depthcharts")
    chart, ranks = {}, {}
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
            for i, a in enumerate(pos.get("athletes", []), 1):
                aid, rank = a["athlete"]["$ref"].split("athletes/")[1].split("?")[0], a.get("rank", i)
                # The best spot at a position wins over any return spot (a WR2 who returns punts is a WR2).
                better = aid not in ranks or (ranks[aid][0] in RETURN_KEYS and key not in RETURN_KEYS) or \
                    ((ranks[aid][0] in RETURN_KEYS) == (key in RETURN_KEYS) and rank < ranks[aid][1])
                if better:
                    ranks[aid] = (key, rank)
    return chart, {aid: f"{k}{r}" for aid, (k, r) in ranks.items()}


RETURN_KEYS = {"kr", "pr", "h", "ls"}  # returners, holder, long snapper: a spot, but not the player's position


def current_week():
    """(season year, label, kind) of the week ESPN's scoreboard is on: 2026-w07, 2026-post1, or 2026-pre."""
    s = get(f"{SITE}/scoreboard")
    year, kind, n = s["season"]["year"], s["season"]["type"], s["week"]["number"]
    if kind == 2:
        return year, f"{year}-w{n:02}", "week"
    if kind == 3:
        return year, f"{year}-post{n}", "week"
    return year, f"{year}-pre", "pre"


def transactions_since(since):
    """Transaction notes (text) since a date: [(date, team, description)], newest first."""
    notes, page = [], 1
    while page <= 20:
        t = get(f"{SITE}/transactions?limit=100&page={page}")
        for x in t.get("transactions", []):
            if x["date"][:10] < since:
                return notes
            notes.append((x["date"][:10], (x.get("team") or {}).get("abbreviation", "?"), x.get("description", "")))
        if page >= t.get("pageCount", 1):
            break
        page += 1
    return notes


def fetch_league_espn(season, with_recent=True, known=None):
    """{franchise: (roster, depth chart, recent starters)} for the 32 clubs, from ESPN."""
    ids = team_ids()
    league = {}
    for f in FRANCHISES:
        if f not in ids:
            sys.exit(f"{f}: not in ESPN's team list")
        tid = ids[f]
        chart, ranks = depth(tid, season)
        league[f] = (club_roster(tid), chart, recent_starters(tid, season) if with_recent else set(), ranks)
        time.sleep(0.2)
    return league


# --- The player file ------------------------------------------------------------------------------------------------

def player_entry(p, team, old=None, rank=None):
    old = old or {}
    e = {
        "first": p["first"], "last": p["last"], "number": p["number"], "pos": p["position"], "team": team,
        "weight": int(p["weight"]), "heavy": heavy_for(5, p), "status": p["status"], "depth": rank,
        "skin": old.get("skin"), "added": old.get("added", today()),
    }
    if old.get("name"):  # a hand-picked spelling (Roster Room) stays
        e["name"] = old["name"]
    return e


def update_players(players, league, numbers=None):
    """Bring the player file up to date with the 32 rosters. Returns notes on what changed."""
    notes = {"new": [], "moved": [], "left": [], "back": []}
    seen = set()
    for f, (roster, _, _, ranks) in league.items():
        for pid, p in roster.items():
            seen.add(pid)
            old = players.get(pid)
            if p["number"] is None:  # just signed, no number on ESPN yet: the last one we knew
                p["number"] = (old or {}).get("number", (numbers or {}).get(pid))
            players[pid] = player_entry(p, f, old, ranks.get(pid))
            who = f"{f} {p['first']} {p['last']} #{p['number']} {p['position']}"
            if old is None:
                notes["new"].append(pid)
            elif old.get("team") is None:
                notes["back"].append(f"{who} (was a free agent)")
            elif old["team"] != f:
                notes["moved"].append(f"{who} (from {old['team']})")
    for pid, e in players.items():
        if pid not in seen and e.get("team") is not None:
            notes["left"].append(f"{e['team']} {e['first']} {e['last']} #{e['number']} {e['pos']}")
            e.update(team=None, status="free", depth=None)
    return notes


def apply_picks(players, picks):
    """Skin shades by ESPN id (picks.json: {id: {"skin": n}}) into the player file. Returns how many changed."""
    n = 0
    for pid, pick in picks.items():
        skin = pick.get("skin") if isinstance(pick, dict) else pick
        if pid in players and skin is not None and players[pid].get("skin") != skin:
            players[pid]["skin"] = skin
            n += 1
    return n


# --- Lineups ----------------------------------------------------------------------------------------------------------

def as_team(slot_ids, roster):
    """A lineup's slots in roster_fetch's team shape (for pick_team's keep-the-current-player rule)."""
    return {"players": [{"slot": s, "number": roster[pid]["number"], "last": roster[pid]["last"]}
                        for s, pid in enumerate(slot_ids) if pid and pid in roster]}


def fill(keep, chart, roster, recent):
    """15 slots: `keep` {slot: id} first, the rest from each group's starters in priority order, then (a stale
    depth chart) anyone healthy on the active roster at the group's positions."""
    pools = {g: starters(chart, roster, g, recent) +
             [p for p in roster.values() if p["position"] in POSITIONS[g] and not p["out"] and p["number"] is not None
              and p["status"] != "practice"] for g in GROUPS}
    used, out = set(keep.values()), dict(keep)
    for group, slot in SLOTS:
        if slot in out:
            continue
        for p in pools[group]:
            if p["id"] not in used:
                out[slot] = p["id"]
                used.add(p["id"])
                break
    return [out.get(s) for s in range(15)]


def week_lineup(prev_ids, chart, roster, recent):
    """Healthy starters this week. A slot's previous player stays while he's a healthy starter in its group
    (defensive slots take any healthy defensive starter), so the lineup only changes where the team did."""
    healthy = {pid: dict(p, out=p["out_week"]) for pid, p in roster.items()}
    pools = {g: starters(chart, healthy, g, recent) for g in GROUPS}
    keep = {}
    for group, slot in SLOTS:
        pid = prev_ids[slot] if prev_ids and slot < len(prev_ids) else None
        pool = pools["dl"] + pools["def"] if group in ("dl", "def") else pools[group]
        if pid and pid not in keep.values() and any(p["id"] == pid for p in pool):
            keep[slot] = pid
    return fill(keep, chart, healthy, recent)


def full_lineup(prev_ids, chart, roster):
    """Full strength: injuries ignored. A slot's previous player stays while he's with the club and either
    hurt (any reserve list, out, or on a recent injury report) or still a starter in its group; a healthy player who's
    lost his job doesn't."""
    everyone = {pid: dict(p, out=False) for pid, p in roster.items() if p["status"] != "practice"}
    pools = {g: starters(chart, everyone, g) for g in GROUPS}
    keep = {}
    for group, slot in SLOTS:
        pid = prev_ids[slot] if prev_ids and slot < len(prev_ids) else None
        if not pid or pid not in everyone or pid in keep.values():
            continue
        pool = pools["dl"] + pools["def"] if group in ("dl", "def") else pools[group]
        # Hurt (out, a reserve list, or on a recent injury report: working back) or still a starter: he stays.
        if roster[pid]["out_week"] or roster[pid].get("hurt_recently") or any(p["id"] == pid for p in pool):
            keep[slot] = pid
    return fill(keep, chart, everyone, set())


# --- The source -------------------------------------------------------------------------------------------------------

SOURCE = "nflverse"


def source():
    """The data source's functions: (fetch_league, current_week, transactions_since, kickoffs)."""
    if SOURCE == "espn":
        def espn_kickoffs(season, label):  # the schedule's kickoffs, so a week freezes the same way (if reachable)
            try:
                import roster_nflverse as nv
                return nv.kickoffs(season, label)
            except Exception as e:
                print(f"no kickoff times ({e}): this week's checkpoint is rewritten for every club")
                return {}
        return fetch_league_espn, current_week, transactions_since, espn_kickoffs
    import roster_nflverse as nv
    return nv.fetch_league, nv.current_week, nv.transactions_since, nv.kickoffs


def lineup_file(kind, label, teams, gaps):
    return {"format": FORMAT, "kind": kind, "week": label, "made": now(), "teams": teams, "gaps": gaps}


def gaps_of(f, ids):
    return [f"{f} slot {s}: nobody" for s, pid in enumerate(ids) if pid is None]


def latest_checkpoint(d):
    files = sorted(p for p in (Path(d) / "lineups").glob("*-*.json") if p.stem != "full")
    return files[-1] if files else None


# --- Manifest -----------------------------------------------------------------------------------------------------

def psp_pack_module():
    """make_pack.py's build(): tools/psp/make_pack.py in blitz, its copy tools/psp_pack.py in blitz-rosters (the cloud
    job's checkout; tools/rosters_publish.sh copies it). Stdlib only."""
    here = Path(__file__).resolve().parent
    for path in (here / "psp_pack.py", here / "psp" / "make_pack.py"):
        if path.exists():
            spec = importlib.util.spec_from_file_location("psp_pack", path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            return mod
    sys.exit(f"no psp_pack.py (or psp/make_pack.py) beside {Path(__file__).name}")


def write_psp_pack(d, latest):
    """psp/pack.txt from the players.json and lineups just written. A pack that differs from the saved one only in its
    header's time is left as it was (as write_json's stamp), so a quiet day changes no hash and no PSP downloads."""
    path = d / PSP_PACK
    if not (d / "players.json").exists() or not (d / "lineups").is_dir():
        return
    data, _, _, _, _ = psp_pack_module().build(d, PSP_PACK_WEEKS, latest=latest, updated=now())
    if path.exists():
        old = path.read_bytes().split(b"\n", 1)
        new = data.split(b"\n", 1)
        if len(old) == 2 and old[1] == new[1] and old[0].split()[:2] == new[0].split()[:2]:
            return
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".part")
    tmp.write_bytes(data)
    tmp.replace(path)


def write_manifest(d, season, latest):
    d = Path(d)
    write_psp_pack(d, latest)
    files = {}
    for p in sorted([d / "players.json", *(d / "lineups").glob("*.json"), d / PSP_PACK]):
        if p.exists():
            files[p.relative_to(d).as_posix()] = hashlib.sha1(p.read_bytes()).hexdigest()
    doc = {"format": FORMAT, "season": season, "updated": now(), "latest": latest, "files": files}
    # The hub's per-device fields (docs/PLATFORMS.md "Content and updates") carry over from the last manifest:
    # "targets" (which platforms or builds take a file) and "app" (the newest app build, announced once per device).
    prev = read_json(d / "manifest.json", {})
    for key in ("targets", "app"):
        if prev.get(key):
            doc[key] = prev[key]
    # Every file's platforms are set here (other target fields, like min_build, carry over): the PSP takes only its
    # pack, the rest only the JSON. A list set with `target --platforms` ("by_hand") is kept as it is. A target for a
    # file no longer published is dropped, unless it was set by hand (it may name a file still to come).
    targets = {k: v for k, v in doc.get("targets", {}).items()
               if isinstance(v, dict) and (k in files or v.get("by_hand"))}
    for path in files:
        t = dict(targets.get(path, {}))
        if not (t.get("by_hand") and isinstance(t.get("platforms"), list)):
            t.pop("by_hand", None)
            t["platforms"] = list(PSP_PLATFORMS if path == PSP_PACK else JSON_PLATFORMS)
        targets[path] = t
    doc["targets"] = targets
    write_json(d / "manifest.json", doc, stamp="updated")


# --- Sanity ---------------------------------------------------------------------------------------------------------

def sanity(league, players_before, players_after, lineups):
    """Reasons not to publish this run (empty: fine). A bad day at the source (a missing club, a half-empty file)
    must not reach a single device: the run fails instead (GitHub emails), nothing is written or uploaded, and every
    device keeps yesterday's rosters."""
    problems = []
    if len(league) != 32:
        problems.append(f"{len(league)} clubs, not 32: {sorted(set(FRANCHISES) - set(league))} missing")
    for f, (roster, chart, _, _) in league.items():
        on_club = sum(1 for p in roster.values() if p["status"] != "practice")
        if on_club < 45:
            problems.append(f"{f}: only {on_club} players on the roster")
        if not chart:
            problems.append(f"{f}: no depth chart")
    before = sum(1 for e in players_before.values() if e.get("team"))
    after = sum(1 for e in players_after.values() if e.get("team"))
    if before and after < before * 0.9:
        problems.append(f"players with a club fell from {before} to {after} (more than 10%)")
    for name, teams in lineups.items():
        holes = sum(1 for ids in teams.values() for pid in ids if pid is None)
        if holes > 8:
            problems.append(f"{name}: {holes} empty slots")
    return problems


# --- Commands -----------------------------------------------------------------------------------------------------

def pack_ids(pack_dir):
    """The pack's 15 slots per team as ESPN ids, where picks.json's "who" (TEAM Last #N) names them."""
    pack = read_json(Path(pack_dir) / "pack.json", {"teams": []})
    picks = read_json(Path(pack_dir) / "picks.json", {})
    by_who = {v.get("who"): k for k, v in picks.items() if isinstance(v, dict)}
    out = {}
    for t in pack["teams"]:
        ids = [None] * 15
        for p in t.get("players", []):
            ids[p["slot"]] = by_who.get(f"{t['franchise']} {p.get('last', '')} #{p.get('number')}")
        out[t["franchise"]] = ids
    return out


def pack_numbers(pack_dir):
    """{ESPN id: number} for the pack's players (picks.json's "who" is TEAM Last #N)."""
    out = {}
    for pid, v in read_json(Path(pack_dir) / "picks.json", {}).items():
        num = v.get("who", "").rpartition("#")[2] if isinstance(v, dict) else ""
        if num.isdigit():
            out[pid] = int(num)
    return out


def report(notes, players):
    for pid in notes["new"]:
        e = players[pid]
        print(f"  new: {e['team']} {e['first']} {e['last']} #{e['number']} {e['pos']} (needs a shade)")
    for k in ("back", "moved", "left"):
        for line in notes[k]:
            print(f"  {k}: {line}")


def cmd_load(a):
    d = Path(a.dir)
    fetch, week_now, _, _ = source()
    season = a.season or week_now()[0]
    players = read_json(d / "players.json", {}).get("players", {})
    league = fetch(season, with_recent=False, known=players)
    notes = update_players(players, league, pack_numbers(a.pack))
    n = apply_picks(players, read_json(Path(a.pack) / "picks.json", {}))
    seeds = pack_ids(a.pack)
    prev_full = read_json(d / "lineups" / "full.json", {}).get("teams", {})
    teams, gaps = {}, []
    for f, (roster, chart, _, _) in league.items():
        teams[f] = full_lineup(prev_full.get(f) or seeds.get(f), chart, roster)
        gaps += gaps_of(f, teams[f])
    write_json(d / "players.json", {"format": FORMAT, "season": season, "made": now(), "players": players})
    write_json(d / "lineups" / "full.json", lineup_file("full", "full", teams, gaps))
    write_manifest(d, season, read_json(d / "manifest.json", {}).get("latest"))
    unshaded = sum(1 for e in players.values() if e.get("skin") is None)
    print(f"{len(players)} players ({len(notes['new'])} new, {n} shades from picks, {unshaded} need a shade); "
          f"full strength written; {len(gaps)} gaps")
    print("\n".join(gaps))


def cmd_week(a):
    d = Path(a.dir)
    fetch, week_now, notes_since, kickoff_of = source()
    season, label, kind = week_now()
    season = a.season or season
    if a.week:
        label, kind = f"{season}-w{int(a.week):02}", "week"
    doc = read_json(d / "players.json")
    if doc is None:
        sys.exit(f"no player file in {d}: run `roster_season.py load` first")
    players = doc["players"]
    players_before = json.loads(json.dumps(players))
    last = read_json(d / "manifest.json", {}).get("updated", today())[:10]
    league = fetch(season, known=players)
    notes = update_players(players, league)

    prev_path = d / "lineups" / f"{label}.json"
    prev = read_json(prev_path) or read_json(latest_checkpoint(d) or prev_path) or read_json(d / "lineups" / "full.json", {})
    prev_full = read_json(d / "lineups" / "full.json", {}).get("teams", {})
    # A club whose game this week has kicked off keeps the slots it had at kickoff (the week's checkpoint is what it
    # played with); one still to play, or on bye, is brought up to date.
    this_week = read_json(prev_path, {}).get("teams", {})
    started = {f for f, t in kickoff_of(season, label).items()
               if t <= datetime.datetime.now(datetime.timezone.utc)} if kind == "week" else set()
    week, full, gaps, fgaps = {}, {}, [], []
    for f, (roster, chart, recent, _) in league.items():
        full[f] = full_lineup(prev_full.get(f), chart, roster)
        fgaps += gaps_of(f, full[f])
        if kind == "week":
            if f in started and this_week.get(f):
                week[f] = this_week[f]
            else:
                week[f] = week_lineup(prev.get("teams", {}).get(f) or full[f], chart, roster, recent)
            gaps += gaps_of(f, week[f])
    problems = sanity(league, players_before, players, {"full strength": full, **({label: week} if kind == "week" else {})})
    if problems and not a.force:
        print(f"{label} ({SOURCE}): NOT published, the data looks wrong:")
        print("\n".join(f"  - {p}" for p in problems))
        print("Every device keeps the last good rosters. Check the source, then rerun (--force publishes anyway).")
        sys.exit(1)
    write_json(d / "players.json", dict(doc, made=now(), players=players), stamp="made")
    write_json(d / "lineups" / "full.json", lineup_file("full", "full", full, fgaps), stamp="made")
    latest = read_json(d / "manifest.json", {}).get("latest")
    if kind == "week":
        cp = lineup_file("week", label, week, gaps)
        cp["notes"] = [f"{dt} {t}: {s}" for dt, t, s in notes_since(last)] or read_json(prev_path, {}).get("notes", [])
        write_json(prev_path, cp, stamp="made")
        latest = label
    write_manifest(d, season, latest)
    print(f"{label} ({SOURCE}): {len(players)} players, {len(notes['new'])} new; {len(gaps)} gaps; "
          f"{len(started)} clubs' week frozen at kickoff")
    report(notes, players)
    print("\n".join(gaps))


def cmd_shades(a):
    d = Path(a.dir)
    doc = read_json(d / "players.json")
    if Path(a.export).is_dir():  # Roster Room's new-face picks: its db collection `shades`, one file per ESPN id
        src = {}
        for f in Path(a.export).glob("*.json"):
            row = json.loads(f.read_text())
            row = row.get("data", row)
            if isinstance(row.get("skin"), int):
                src[f.stem] = {"skin": row["skin"]}
    else:
        src = json.loads(Path(a.export).read_text())
    if "teams" in src:  # a Roster Room pack export: players carry "espn" ids where the room knew them
        picks = {str(p["espn"]): {"skin": p["skin"]} for t in src["teams"] for p in t.get("players", [])
                 if p.get("espn") and p.get("skin") is not None}
        picks.update({str(k): {"skin": v} for k, v in src.get("shades", {}).items()})
    else:
        picks = src
    n = apply_picks(doc["players"], picks)
    write_json(d / "players.json", dict(doc, made=now()))
    write_manifest(d, doc["season"], read_json(d / "manifest.json", {}).get("latest"))
    print(f"{n} shades set")


def cmd_new(a):
    d = Path(a.dir)
    players = read_json(d / "players.json")["players"]
    slotted = set()
    for p in (d / "lineups").glob("*.json"):
        for ids in read_json(p)["teams"].values():
            slotted.update(i for i in ids if i)
    need = [(pid, e) for pid, e in players.items() if e.get("skin") is None and e.get("team")]
    need.sort(key=lambda x: (x[0] not in slotted, x[1]["team"], x[1]["last"]))
    for pid, e in need:
        star = "*" if pid in slotted else " "
        print(f"{star} {pid:>8} {e['team']:>3} {e['first']} {e['last']} #{e['number']} {e['pos']}")
    print(f"{len(need)} need a shade; * = in a lineup")


def cmd_announce(a):
    """The manifest's "app" block: every device on an older build shows "Update available" once."""
    d = Path(a.dir)
    m = read_json(d / "manifest.json")
    m["app"] = {k: v for k, v in {"build": a.build, "version": a.version, "notes": a.notes,
                                   "platforms": a.platforms.split(",") if a.platforms else None}.items() if v}
    write_json(d / "manifest.json", m)
    print(f"announced build {a.build}" + (f" for {a.platforms}" if a.platforms else ""))


def cmd_target(a):
    """Limit a published file to some platforms (names as include/blitz_platform.h) or builds; --clear lifts it (the
    next job run puts the default platforms back: JSON_PLATFORMS, or PSP_PLATFORMS for the pack)."""
    d = Path(a.dir)
    m = read_json(d / "manifest.json")
    targets = m.setdefault("targets", {})
    if a.clear:
        targets.pop(a.path, None)
    else:
        # by_hand: the job (write_manifest) keeps this platform list instead of resetting it to the defaults.
        targets[a.path] = {k: v for k, v in {"platforms": a.platforms.split(",") if a.platforms else None,
                                             "by_hand": True if a.platforms else None,
                                             "min_build": a.min_build}.items() if v}
    if not targets:
        m.pop("targets")
    write_json(d / "manifest.json", m)
    print(json.dumps(m.get("targets", {}), indent=1))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("load", "week", "shades", "new"):
        s = sub.add_parser(name)
        s.add_argument("--dir", default=str(ROOT / "build" / "rosters"))
        if name in ("load", "week"):
            s.add_argument("--season", type=int)
        if name == "load":
            s.add_argument("--pack", default=str(ROOT / "packs" / "blitz2026"))
        if name == "week":
            s.add_argument("--week", type=int)
            s.add_argument("--force", action="store_true", help="publish even if the sanity checks fail")
        if name == "shades":
            s.add_argument("export")
    s = sub.add_parser("announce", help="tell every device on an older build that a new app build is out")
    s.add_argument("--dir", default=str(ROOT / "build" / "rosters"))
    s.add_argument("--build", type=int, required=True)
    s.add_argument("--version")
    s.add_argument("--notes")
    s.add_argument("--platforms", help="comma-separated (macOS,Windows,Linux,iOS,Android,PS5); default all")
    s = sub.add_parser("target", help="limit a published file to some platforms or builds")
    s.add_argument("--dir", default=str(ROOT / "build" / "rosters"))
    s.add_argument("path")
    s.add_argument("--platforms")
    s.add_argument("--min-build", type=int)
    s.add_argument("--clear", action="store_true")
    for sp in sub.choices.values():
        sp.add_argument("--source", choices=("nflverse", "espn"), default="nflverse",
                        help="where the rosters come from (espn: the original flow, kept as a backup)")
    a = ap.parse_args(argv)
    global SOURCE
    SOURCE = a.source
    {"load": cmd_load, "week": cmd_week, "shades": cmd_shades, "new": cmd_new, "announce": cmd_announce,
     "target": cmd_target}[a.cmd](a)


if __name__ == "__main__":
    main()
