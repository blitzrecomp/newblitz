#!/usr/bin/env python3
"""Decode an Advanced-mode Season from an RDRAM dump of the 2026 flavour (tools/live.sh dump).

  season_dump.py DUMP.bin [--pack packs/blitz2026] [--weeks N] [--stats] [--json]
  season_dump.py DUMP.bin --classic     the game's own Season (Classic, or Standard in the 2026 app):
                                        records per division and the 6 seeds per conference; 32 teams
                                        and 16-game weeks when the record has the "S32" marker
                                        (src/game/season32.cpp), with team 31's stats

Prints the user's team and week, every played week's results (matched to the pack's schedule; team
indices follow the pack's "order", as in Advanced), division standings, each team's stats row
(flagging non-finite floats) and the playoff seeds and games.

Record layout in Advanced (src/game/adv_engine.cpp): +0 user team, +1 week (0-17 regular, 18-21
playoffs, 22 over), +4 stats f32[31][7] (team 31's at +0x704), +0x37F results [22][16] {home score,
away score} (0xFF = not played), +0x758 seeds [2][7]. Dumps are the runtime's RDRAM: 32-bit words in
host (little-endian) order, so byte a lives at (a - 0x80000000) ^ 3.
"""
import argparse
import json
import math
import struct
import sys
from pathlib import Path

RECORD = 0x802EF6F0
RESULTS, STATS, STATS31, SEEDS = 0x37F, 0x4, 0x704, 0x758
REGULAR, TOTAL, MAX_GAMES = 18, 22, 16
ROUNDS = ["Wildcard", "Divisional", "Championship", "Super Bowl"]


class Ram:
    def __init__(self, data):
        self.d = data

    def b(self, a):
        return self.d[(a - 0x80000000) ^ 3]

    def w(self, a):
        return struct.unpack_from("<I", self.d, a - 0x80000000)[0]

    def f(self, a):
        return struct.unpack("<f", struct.pack("<I", self.w(a)))[0]


def playoff_games(seeds, score, week):
    """Mirror of adv::playoff_games (src/game/adv_league.cpp)."""
    def seed_of(c, t):
        return seeds[c].index(t) if t in seeds[c] else 7

    def host(c, a, b):
        return (b, a) if seed_of(c, a) < seed_of(c, b) else (a, b)

    def winners(rnd, c):
        out = []
        for g, (away, home) in enumerate(playoff_games(seeds, score, rnd)):
            s = score[rnd][g]
            if 0xFF in s:
                continue
            win = home if s[0] > s[1] else away
            if seed_of(c, away) < 7 or seed_of(c, home) < 7:
                out.append(win)
        return out

    games = []
    if week == 18:
        for c in (0, 1):
            s = seeds[c]
            games += [(s[6], s[1]), (s[5], s[2]), (s[4], s[3])]
    elif week == 19:
        for c in (0, 1):
            left = sorted(winners(18, c), key=lambda t: seed_of(c, t))
            if len(left) == 3:
                games += [host(c, seeds[c][0], left[2]), host(c, left[0], left[1])]
    elif week == 20:
        for c in (0, 1):
            left = winners(19, c)
            if len(left) == 2:
                games.append(host(c, left[0], left[1]))
    elif week == 21:
        a, n = winners(20, 0), winners(20, 1)
        if len(a) == 1 and len(n) == 1:
            games.append((a[0], n[0]))
    return games


def classic(r):
    """The game's 1999 league: division map 0x8032E898, week blocks via 0x8032E844 (ovl_b loaded)."""
    s32 = bytes(r.b(RECORD + 0x720 + k) for k in range(4)) == b"S32\x01"
    teams, games = (32, 16) if s32 else (31, 15)
    pos = {}
    for t in range(teams):
        d, s = r.b(0x8032E898 + 2 * t), r.b(0x8032E899 + 2 * t)
        pos[(d, s)] = t
    w_, l_ = [0] * teams, [0] * teams
    week = r.b(RECORD + 1)
    per_week = []
    for w in range(min(week, 17)):
        blk = r.w(0x8032E844 + 4 * w)
        n = 0
        for g in range(games):
            e = [r.b(blk + 5 * g + k) for k in range(4)]
            if e[2] == 0xFF:
                break
            away, home = pos.get((e[0], e[1])), pos.get((e[2], e[3]))
            hs, as_ = r.b(RECORD + 0x380 + 47 * w + 3 * g), r.b(RECORD + 0x381 + 47 * w + 3 * g)
            if away is None or home is None or 0xFF in (hs, as_):
                continue
            n += 1
            win, lose = (home, away) if hs > as_ else (away, home)
            w_[win] += 1
            l_[lose] += 1
        per_week.append(n)
    print(f"{'32-team (S32)' if s32 else '31-team (team+)'} season: user team {r.b(RECORD)}, week index {week}, "
          f"{len(pos)} teams placed, games scored per week {per_week}")
    if s32:
        stats = [r.f(RECORD + 0x704 + 4 * k) for k in range(7)]
        print("team 31 stats: " + " ".join(f"{x:g}" for x in stats) + ("" if all(math.isfinite(x) for x in stats) else "  NOT FINITE"))
    for d in range(6):
        members = sorted((s, t) for (dd, s), t in pos.items() if dd == d)
        print(f"division {d}: " + "  ".join(f"#{t} {w_[t]}-{l_[t]}" for _, t in members))
    for c in (0, 1):
        seeds = [r.b(RECORD + 0x758 + 6 * c + i) for i in range(6)]
        print(f"conference {c} seeds: " + "  ".join(f"#{t} {w_[t]}-{l_[t]} (div {next(d for (d, s), tt in pos.items() if tt == t)})"
                                                    if t < teams else "--" for t in seeds))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--pack", default=str(Path(__file__).resolve().parent.parent / "packs" / "blitz2026"))
    ap.add_argument("--weeks", type=int, default=None, help="show at most N weeks of results")
    ap.add_argument("--stats", action="store_true", help="print every team's stats row")
    ap.add_argument("--json", action="store_true", help="machine-readable summary")
    ap.add_argument("--classic", action="store_true", help="the game's own 31-team Season")
    args = ap.parse_args(argv)
    r = Ram(open(args.dump, "rb").read())
    if args.classic:
        return classic(r)
    pack_file = Path(args.pack)
    pack = json.loads((pack_file / "pack.json" if pack_file.is_dir() else pack_file).read_text())
    names = pack["order"]
    idx = {f: i for i, f in enumerate(names)}
    season = pack["season"]
    schedule = [[(idx[a], idx[h]) for a, h in week] for week in season["schedule"]]

    team, week = r.b(RECORD), r.b(RECORD + 1)
    score = [[(r.b(RECORD + RESULTS + (w * MAX_GAMES + g) * 2), r.b(RECORD + RESULTS + (w * MAX_GAMES + g) * 2 + 1))
              for g in range(MAX_GAMES)] for w in range(TOTAL)]
    seeds = [[r.b(RECORD + SEEDS + c * 7 + i) for i in range(7)] for c in (0, 1)]
    out = {"team": team, "team_name": names[team] if team < 32 else None, "week": week, "weeks": [], "problems": []}

    for w in range(TOTAL):
        games = schedule[w] if w < REGULAR else (playoff_games(seeds, score, w) if all(s < 32 for c in seeds for s in c) else [])
        played = [{"away": a, "home": h, "away_score": score[w][g][1], "home_score": score[w][g][0]}
                  for g, (a, h) in enumerate(games) if 0xFF not in score[w][g]]
        if w < week and len(played) != len(games):
            out["problems"].append(f"week {w + 1}: {len(games) - len(played)} game(s) without a score")
        if played:
            out["weeks"].append({"week": w, "games": played})

    stats = []
    for t in range(32):
        base = RECORD + (STATS31 if t == 31 else STATS + 28 * t)
        row = [r.f(base + 4 * k) for k in range(7)]
        stats.append(row)
        if any(not math.isfinite(v) for v in row):
            out["problems"].append(f"{names[t]} (team {t}) stats not finite: {row}")
    out["stats"] = stats
    out["seeds"] = seeds

    if args.json:
        json.dump(out, sys.stdout, indent=1)
        print()
        return 1 if out["problems"] else 0

    print(f"user team {team} ({out['team_name']}), week index {week}")
    shown = out["weeks"][:args.weeks] if args.weeks is not None else out["weeks"]
    for wk in shown:
        w = wk["week"]
        label = f"week {w + 1:2}" if w < REGULAR else ROUNDS[w - REGULAR]
        line = "  ".join(f"{names[g['away']]} {g['away_score']} @ {names[g['home']]} {g['home_score']}" for g in wk["games"])
        print(f"{label} ({len(wk['games'])}): {line}")
    wins = [0] * 32
    losses = [0] * 32
    for wk in out["weeks"]:
        if wk["week"] >= REGULAR:
            continue
        for g in wk["games"]:
            hw = g["home_score"] > g["away_score"]
            wins[g["home"] if hw else g["away"]] += 1
            losses[g["away"] if hw else g["home"]] += 1
    for d in season["divisions"]:
        print(f"{d['name']:10} " + "  ".join(f"{f} {wins[idx[f]]}-{losses[idx[f]]}" for f in d["teams"]))
    if args.stats:
        for t in range(32):
            print(f"{t:2} {names[t]:4} " + " ".join(f"{v:7.1f}" for v in stats[t]))
    for c, conf in enumerate(("AFC", "NFC")):
        print(f"{conf} seeds: " + " ".join(names[s] if s < 32 else "--" for s in seeds[c]))
    for p in out["problems"]:
        print("PROBLEM:", p)
    return 1 if out["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
