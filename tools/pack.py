#!/usr/bin/env python3
"""Content packs ("blitz-pack/1") for the native port: validate, inspect, start one.

  pack.py validate PACK          check a pack.json (or its folder) with the same rules as the game
                                 (blitz-pack/1 for Classic, blitz-pack/2 for the 2026 build)
  pack.py show TEAM [--rom R]    print a team's vanilla ratings and roster from the ROM
  pack.py template TEAM [--rom R] [--id ID]
                                 print a starter pack for TEAM with every player's vanilla values
                                 as "expect" guards (edit, then drop the fields you don't change)

TEAM is 0-30 or a 1999 abbreviation (ARZ, G.B or GB, ...). The rules mirror
src/game/content_pack.cpp; the layouts are in docs/CONTENT_MAP.md. The ROM is only read.
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

TEAM_ABBRS = ["ARZ", "ATL", "BAL", "BUF", "CAR", "CHI", "CIN", "CLE", "DAL", "DEN", "DET", "G.B", "IND", "JAC", "K.C", "MIA",
              "MIN", "N.E", "N.O", "NYG", "NYJ", "OAK", "PHI", "PIT", "S.D", "S.F", "SEA", "STL", "T.B", "TEN", "WAS"]
RATINGS = ["passing", "rushing", "linemen", "defense", "special_teams"]
# Player skin shade ("skin"), light to dark: 5 Lighter, 1 Light, 2 Light-medium, 3 Medium, 4 Medium-dark,
# 0 Darker. 1 and 0 are the game's own two texture sets; 2-5 are drawn by src/game/skin_tones.cpp from the
# ROM's own skin palettes.
SKIN_MAX = 5
SKIN_SHADES = {5: "Lighter", 1: "Light", 2: "Light-medium", 3: "Medium", 4: "Medium-dark", 0: "Darker"}
SLOT_ROLES = ["OL", "OL", "OL", "QB", "RB", "WR/TE", "WR/TE", "DL", "DL", "DL", "DB/LB", "DB/LB", "DB/LB", "DB/LB", "K", "(empty)"]

# ROM offsets (USA v1.1): ovl_b data, see docs/CONTENT_MAP.md.
ROM_TEAMS, TEAM_STRIDE = 0x1264EC, 0x64
ROM_ROSTERS, PLAYER_STRIDE, PER_TEAM = 0x11F0AC, 0x3C, 16
ROM_SIZE = 0x1000000

NAME_RE = re.compile(r"^[A-Za-z .'-]+$")
TEXT_RE = re.compile(r"^[A-Za-z0-9 .'-]+$")
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,40}$")


class PackError(Exception):
    pass


def team_index(value, where="team"):
    if isinstance(value, bool):
        raise PackError(f"{where}: unknown team {value!r}")
    if isinstance(value, int):
        if 0 <= value < len(TEAM_ABBRS):
            return value
        raise PackError(f"{where}: team index must be 0-30")
    if isinstance(value, str):
        want = value.replace(".", "").upper()
        for i, abbr in enumerate(TEAM_ABBRS):
            if abbr.replace(".", "") == want:
                return i
    raise PackError(f"{where}: unknown team {value!r} (use 0-30 or the 1999 abbreviation, e.g. \"ARZ\", \"G.B\")")


def only_keys(obj, allowed, where):
    if not isinstance(obj, dict):
        raise PackError(f"{where} must be an object")
    for k in obj:
        if k not in allowed:
            raise PackError(f"{where}: unknown key \"{k}\"")


def str_field(obj, key, max_len, name, where):
    if key not in obj:
        raise PackError(f"{where}: missing \"{key}\"")
    s = obj[key]
    if not isinstance(s, str):
        raise PackError(f"{where}: \"{key}\" must be a string")
    if not 1 <= len(s) <= max_len:
        raise PackError(f"{where}: \"{key}\" must be 1-{max_len} characters (\"{s}\")")
    if not (NAME_RE if name else TEXT_RE).match(s):
        raise PackError(f"{where}: \"{key}\" has characters the game can't draw (\"{s}\")")
    return s


def int_field(obj, key, lo, hi, where):
    if key not in obj:
        raise PackError(f"{where}: missing \"{key}\"")
    v = obj[key]
    if isinstance(v, bool) or not isinstance(v, int):
        raise PackError(f"{where}: \"{key}\" must be a whole number")
    if not lo <= v <= hi:
        raise PackError(f"{where}: \"{key}\" must be {lo}-{hi} (got {v})")
    return v


def validate(root):
    """Raise PackError for anything the game would reject; return a summary of what changes."""
    if isinstance(root, dict) and root.get("format") == "blitz-pack/2":
        return validate_v2(root)
    changes = []
    only_keys(root, {"format", "id", "title", "requires_rom", "teams"}, "pack")
    if root.get("format") != "blitz-pack/1":
        raise PackError("\"format\" must be \"blitz-pack/1\" (Classic) or \"blitz-pack/2\" (2026 build)")
    if root.get("requires_rom") != "blitz2000-v11":
        raise PackError("\"requires_rom\" must be \"blitz2000-v11\" (NFL Blitz 2000 USA v1.1)")
    if not isinstance(root.get("id"), str) or not ID_RE.match(root["id"]):
        raise PackError("\"id\" must be 1-40 letters, digits, '-' or '_'")
    if not isinstance(root.get("teams"), list):
        raise PackError("\"teams\" must be a list")
    seen_teams = set()
    for t in root["teams"]:
        only_keys(t, {"team", "ratings", "city", "city_upper", "city_abbr", "players"}, "team entry")
        if "team" not in t:
            raise PackError("team entry: missing \"team\"")
        ti = team_index(t["team"], "team entry")
        where = f"team {TEAM_ABBRS[ti]}"
        if ti in seen_teams:
            raise PackError(f"{where} appears twice")
        seen_teams.add(ti)
        if "ratings" in t:
            only_keys(t["ratings"], set(RATINGS), f"{where} ratings")
            for k in RATINGS:
                if k in t["ratings"]:
                    changes.append(f"{where} {k} = {int_field(t['ratings'], k, 0, 5, where + ' ratings')}")
        if "city" in t:
            city = str_field(t, "city", 31, False, where)
            upper = (str_field(t, "city_upper", 15, False, where) if "city_upper" in t else city).upper()
            if len(upper) > 15:
                raise PackError(f"{where}: the city is over 15 characters; give a shorter \"city_upper\" for team select")
            changes.append(f"{where} city = {city!r} (team select {upper!r})")
        elif "city_upper" in t:
            raise PackError(f"{where}: \"city_upper\" needs \"city\"")
        if "city_abbr" in t:
            changes.append(f"{where} city_abbr = {str_field(t, 'city_abbr', 3, False, where)!r}")
        if "players" in t:
            if not isinstance(t["players"], list):
                raise PackError(f"{where}: \"players\" must be a list")
            seen_slots = set()
            for p in t["players"]:
                only_keys(p, {"slot", "expect", "first", "last", "number", "size", "skin", "body", "announcer_call"}, f"{where} player")
                slot = int_field(p, "slot", 0, PER_TEAM - 1, f"{where} player")
                pw = f"{where} slot {slot}"
                if slot in seen_slots:
                    raise PackError(f"{pw} appears twice")
                seen_slots.add(slot)
                expect = {}
                if "expect" in p:
                    e = p["expect"]
                    only_keys(e, {"first", "last", "number"}, f"{pw} expect")
                    for k in ("first", "last"):
                        if k in e:
                            expect[k] = str_field(e, k, 15, True, f"{pw} expect")
                    if "number" in e:
                        int_field(e, "number", 0, 99, f"{pw} expect")
                # A name counts as changed unless it equals the pack's own "expect" for that field.
                renamed = False
                for k in ("first", "last"):
                    if k in p:
                        value = str_field(p, k, 15, True, pw)
                        if k not in expect or value.lower() != expect[k].lower():
                            renamed = True
                            changes.append(f"{pw} {k} = {value!r}")
                if "number" in p:
                    changes.append(f"{pw} number = {int_field(p, 'number', 0, 99, pw)}")
                if "size" in p:
                    size = p["size"]
                    if isinstance(size, bool) or not isinstance(size, (int, float)):
                        raise PackError(f"{pw}: \"size\" must be a number")
                    if not 0.9 <= size <= 1.3:
                        raise PackError(f"{pw}: \"size\" must be 0.90-1.30 (vanilla 1.05)")
                    changes.append(f"{pw} size = {size}")
                if "skin" in p:
                    changes.append(f"{pw} skin = {int_field(p, 'skin', 0, SKIN_MAX, pw)}")
                if "body" in p:
                    changes.append(f"{pw} body = {int_field(p, 'body', 0, 1, pw)}")
                call = p.get("announcer_call", "none" if renamed else "keep")
                if call not in ("none", "keep"):
                    raise PackError(f"{pw}: \"announcer_call\" must be \"none\" or \"keep\"")
                if call == "keep" and renamed:
                    raise PackError(f"{pw}: a renamed player can't keep the old announcer name call (use \"none\")")
    return changes


# --- blitz-pack/2 (the 2026 build, team+ blocks; src/game/content_pack_tp.cpp) ---------------

FRANCHISES = ["ARI", "ATL", "BAL", "BUF", "CAR", "CHI", "CIN", "CLE", "DAL", "DEN", "DET", "GB", "HOU", "IND", "JAX", "KC",
              "LV", "LAC", "LAR", "MIA", "MIN", "NE", "NO", "NYG", "NYJ", "PHI", "PIT", "SF", "SEA", "TB", "TEN", "WAS"]


def validate_season(s):
    """The 2026 build's "season" section (Advanced mode): 8 divisions of 4, up to 18 weeks."""
    only_keys(s, {"year", "divisions", "schedule"}, "season")
    year = int_field(s, "year", 2000, 2100, "season")
    divs = s.get("divisions")
    if not isinstance(divs, list) or len(divs) != 8:
        raise PackError("season: \"divisions\" must list 8 divisions (AFC East, North, South, West, then the NFC's)")
    placed = []
    for d, div in enumerate(divs):
        where = f"season division {d + 1}"
        only_keys(div, {"name", "teams"}, where)
        str_field(div, "name", 15, False, where)
        teams = div.get("teams")
        if not isinstance(teams, list) or len(teams) != 4 or any(f not in FRANCHISES for f in teams):
            raise PackError(f"{where}: \"teams\" must list 4 franchise ids")
        placed += teams
    if len(set(placed)) != 32:
        raise PackError("season: every franchise must be in exactly one division")
    sched = s.get("schedule")
    if not isinstance(sched, list) or not 1 <= len(sched) <= 18:
        raise PackError("season: \"schedule\" must list 1-18 weeks")
    games = {f: 0 for f in FRANCHISES}
    for w, week in enumerate(sched):
        where = f"season week {w + 1}"
        if not isinstance(week, list) or len(week) > 16:
            raise PackError(f"{where} must be a list of at most 16 games")
        playing = set()
        for g in week:
            if not (isinstance(g, list) and len(g) == 2 and all(f in FRANCHISES for f in g)):
                raise PackError(f"{where}: each game must be [away, home] franchise ids (got {g!r})")
            for f in g:
                if f in playing:
                    raise PackError(f"{where}: {f} plays twice")
                playing.add(f)
                games[f] += 1
    counts = sorted(set(games.values()))
    return [f"season {year}: 8 divisions, {len(sched)} weeks, {sum(games.values()) // 2} games, games per team {counts}"]


def validate_v2(root):
    changes = []
    only_keys(root, {"format", "id", "title", "version", "requires_rom", "order", "teams", "season"}, "pack")
    if "version" in root and not isinstance(root["version"], str):
        raise PackError("\"version\" must be text, e.g. \"2026.10.16\"")
    if root.get("requires_rom") != "blitz2000-teamplus":
        raise PackError("\"requires_rom\" must be \"blitz2000-teamplus\"")
    if not isinstance(root.get("id"), str) or not ID_RE.match(root["id"]):
        raise PackError("\"id\" must be 1-40 letters, digits, '-' or '_'")
    if "order" in root:
        order = root["order"]
        if not isinstance(order, list) or any(f not in FRANCHISES for f in order):
            raise PackError("\"order\": every entry must be a franchise id (" + " ".join(FRANCHISES) + ")")
        if len(set(order)) != len(order):
            raise PackError("\"order\": a franchise appears twice")
        if len(order) != len(FRANCHISES):
            raise PackError(f"\"order\" must list all {len(FRANCHISES)} franchises")
        changes.append("menu order: " + " ".join(order))
    if "season" in root:
        changes += validate_season(root["season"])
    seen = set()
    for t in root.get("teams", []):
        only_keys(t, {"franchise", "city", "nickname", "select", "abbr", "ratings", "players"}, "team entry")
        f = t.get("franchise")
        if f not in FRANCHISES:
            raise PackError(f"team entry: \"franchise\" must be one of {' '.join(FRANCHISES)}")
        where = f"team {f}"
        if f in seen:
            raise PackError(f"{where} appears twice")
        seen.add(f)
        for k, n in (("city", 15), ("nickname", 15), ("select", 15), ("abbr", 3)):
            if k in t:
                changes.append(f"{where} {k} = {str_field(t, k, n, False, where)!r}")
        if "ratings" in t:
            only_keys(t["ratings"], set(RATINGS), f"{where} ratings")
            for k in RATINGS:
                if k in t["ratings"]:
                    changes.append(f"{where} {k} = {int_field(t['ratings'], k, 0, 5, where + ' ratings')}")
        slots = set()
        for p in t.get("players", []):
            only_keys(p, {"slot", "expect", "last", "number", "size", "skin", "heavy", "announcer_call"}, f"{where} player")
            slot = int_field(p, "slot", 0, PER_TEAM - 1, f"{where} player")
            pw = f"{where} slot {slot}"
            if slot in slots:
                raise PackError(f"{pw} appears twice")
            slots.add(slot)
            if "expect" in p:
                only_keys(p["expect"], {"last"}, f"{pw} expect")
                str_field(p["expect"], "last", 15, True, f"{pw} expect")
            if "last" in p:
                changes.append(f"{pw} last = {str_field(p, 'last', 15, True, pw)!r}")
            if "number" in p:
                changes.append(f"{pw} number = {int_field(p, 'number', 0, 99, pw)}")
            if "skin" in p:
                int_field(p, "skin", 0, SKIN_MAX, pw)
            if "heavy" in p:
                int_field(p, "heavy", 0, 1, pw)
            if "size" in p:
                size = p["size"]
                if isinstance(size, bool) or not isinstance(size, (int, float)) or not 0.9 <= size <= 1.3:
                    raise PackError(f"{pw}: \"size\" must be 0.90-1.30 (vanilla 1.05)")
            call = p.get("announcer_call", "none" if "last" in p else "keep")
            if call not in ("none", "keep"):
                raise PackError(f"{pw}: \"announcer_call\" must be \"none\" or \"keep\"")
    return changes


def load_pack_file(path):
    path = Path(path)
    if path.is_dir():
        path = path / "pack.json"
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as e:
        raise PackError(f"not valid JSON: {e}")


# --- ROM reading (show / template) -----------------------------------------------------------

def cstr(data, off, n):
    raw = data[off:off + n]
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def read_team(rom, ti):
    t = ROM_TEAMS + ti * TEAM_STRIDE
    ratings = struct.unpack_from(">5h", rom, t)
    team = {
        "index": ti, "abbr": TEAM_ABBRS[ti],
        "ratings": dict(zip(RATINGS, ratings)),
        "nickname": cstr(rom, t + 0x10, 32), "city": cstr(rom, t + 0x30, 32), "city_abbr": cstr(rom, t + 0x50, 4),
        "players": [],
    }
    for s in range(PER_TEAM):
        r = ROM_ROSTERS + (ti * PER_TEAM + s) * PLAYER_STRIDE
        size, body, skin, num_bcd, pos = struct.unpack_from(">f4I", rom, r)
        team["players"].append({
            "slot": s, "role": SLOT_ROLES[s], "first": cstr(rom, r + 0x2C, 16), "last": cstr(rom, r + 0x1C, 16),
            "number": (num_bcd >> 4) * 10 + (num_bcd & 0xF), "size": round(size, 3), "skin": skin, "body": body,
            "position": pos,
        })
    return team


def load_rom(path):
    rom = Path(path).read_bytes()
    if len(rom) != ROM_SIZE or rom[0x20:0x27] != b"blitz2k":
        raise SystemExit(f"{path}: not the NFL Blitz 2000 .z64 this tool expects (run `make rom-import`)")
    if cstr(rom, ROM_TEAMS + 0x10, 32) != "Cardinals":
        raise SystemExit(f"{path}: team table not found where USA v1.1 has it")
    return rom


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("validate")
    v.add_argument("pack")
    for name in ("show", "template"):
        s = sub.add_parser(name)
        s.add_argument("team")
        s.add_argument("--rom", default=str(Path(__file__).resolve().parent.parent / "build" / "baserom.z64"))
        if name == "template":
            s.add_argument("--id", default="my-pack")
    args = ap.parse_args(argv)

    if args.cmd == "validate":
        try:
            changes = validate(load_pack_file(args.pack))
        except PackError as e:
            print(f"invalid: {e}")
            return 1
        print(f"ok: {len(changes)} change(s)")
        for c in changes:
            print("  " + c)
        return 0

    rom = load_rom(args.rom)
    try:
        ti = team_index(int(args.team) if args.team.isdigit() else args.team)
    except PackError as e:
        raise SystemExit(str(e))
    team = read_team(rom, ti)
    if args.cmd == "show":
        r = team["ratings"]
        print(f"{team['abbr']} ({ti}) {team['city']} {team['nickname']}  " + " ".join(f"{k}={r[k]}" for k in RATINGS))
        for p in team["players"]:
            print(f"  slot {p['slot']:2} {p['role']:7} #{p['number']:<2} {p['first']} {p['last']}  size={p['size']} skin={p['skin']} body={p['body']}")
        return 0
    players = []
    for p in team["players"]:
        entry = {"slot": p["slot"]}
        if p["last"]:
            entry["expect"] = {"first": p["first"], "last": p["last"], "number": p["number"]}
            entry.update({"first": p["first"], "last": p["last"], "number": p["number"]})
        players.append(entry)
    pack = {"format": "blitz-pack/1", "id": args.id, "title": args.id, "requires_rom": "blitz2000-v11",
            "teams": [{"team": team["abbr"], "ratings": team["ratings"], "players": players}]}
    print(json.dumps(pack, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
