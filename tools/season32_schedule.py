#!/usr/bin/env python3
"""Standard Season with 32 teams (2026 app): a 17-week schedule in the game's own week-block format.

Our own generated schedule (no game data). `--inc src/game/season32_schedule.inc` writes the table
src/game/season32.cpp copies into ovl_b's week blocks (docs/BLITZ2026.md, "Season with 32 teams").
First used by Advanced mode's Stage A (commit 8d95ad1), where this exact table (--seed 2026) was tested.

Teams are (division, slot) pairs, exactly as the game's schedule data names them; the division map
(team index -> div, slot) is separate, so this table is independent of team order and of any ROM.
Divisions: 0-2 AFC, 3-5 NFC, sizes 5/6/5 and 5/5/6 (the 32nd team is NFC div 5 slot 5, or e.g. HOU
at AFC div 2 slot 2 with SEA moved to div 5 slot 5 -- either way the division SIZES are these).

Week block (0x64 bytes, pointer table at ovl_b 0x8032E844):
  +0x00  game[i] = {divA, slotA, divB, slotB, flags}, 5 bytes; list ends at the first entry whose
         byte +2 is 0xFF (vanilla filler FF FF FF FF 80)
  +0x50  bye[k] = {s16 div, s16 slot}, ends with div = -1      (vanilla +0x4C; the week builder's hook
                                                                at 0x802FE234 reads byes at +0x50)
16 games need 80 bytes, so 16-game weeks have no byes and 14-game weeks have 4 byes (+terminator =
20 bytes = exactly 0x64). Rules (vanilla-like): home-and-away vs every division rival; 5-team-division
teams 4 more in-conference + 4 cross-conference; 6-team-division teams 3 + 3; 8 home games each
(side A = home, as in vanilla where every team is side A exactly 8 times); byes in weeks 3-10.

usage: season32_schedule.py [--seed N] [--hex] [--inc PATH]
"""
import random
import sys
from collections import Counter, defaultdict

SIZES = [5, 6, 5, 5, 5, 6]
TEAMS = [(d, s) for d, n in enumerate(SIZES) for s in range(n)]
CONF = lambda t: 0 if t[0] < 3 else 1
WEEKS, BYE_WEEKS = 17, list(range(3, 11))


def pairings(rng):
    """Multiset of games (unordered) meeting the degree rules; returns list of (a, b)."""
    games = []
    for d, n in enumerate(SIZES):
        for i in range(n):
            for j in range(i + 1, n):
                games += [((d, i), (d, j))] * 2
    need = {t: (4 if SIZES[t[0]] == 5 else 3) for t in TEAMS}
    # in-conference, other division: simple graph per conference; then cross-conference bipartite
    for conf in (0, 1):
        vs = [t for t in TEAMS if CONF(t) == conf]
        games += _realise(vs, dict((t, need[t]) for t in vs), lambda a, b: a[0] != b[0], rng)
    games += _realise(TEAMS, dict(need), lambda a, b: CONF(a) != CONF(b), rng)
    return games


def _realise(vs, deg, allowed, rng):
    """Havel-Hakimi style: random simple graph on vs with these degrees and allowed pairs."""
    for _ in range(10000):
        d = dict(deg)
        edges, ok = set(), True
        while any(d.values()):
            t = max(vs, key=lambda x: (d[x], rng.random()))
            cand = [o for o in vs if o != t and d[o] > 0 and allowed(t, o) and frozenset((t, o)) not in edges]
            if len(cand) < d[t]:
                ok = False
                break
            cand.sort(key=lambda o: (-d[o], rng.random()))
            for o in cand[:d[t]]:
                edges.add(frozenset((t, o)))
                d[o] -= 1
            d[t] = 0
        if ok:
            return [tuple(sorted(e)) for e in edges]
    raise SystemExit("no pairing found")


def assign_weeks(games, rng, tries=20000):
    """Week by week, a perfect matching of the teams not on bye, drawn from the remaining games
    (most-constrained team first, random tie-break); restart on a dead end. Bye groups of 4 in
    weeks 3-10. Returns the week of each game (index-aligned with games)."""
    for _ in range(tries):
        order = TEAMS[:]
        rng.shuffle(order)
        bye_of = {t: BYE_WEEKS[i // 4] for i, t in enumerate(order)}
        remaining = defaultdict(list)  # team -> indices of unscheduled games
        for gi, (a, b) in enumerate(games):
            remaining[a].append(gi)
            remaining[b].append(gi)
        week = [None] * len(games)
        weeks = list(range(WEEKS))
        rng.shuffle(weeks)
        weeks.sort(key=lambda w: w not in BYE_WEEKS)  # bye weeks first (smaller matchings)
        ok = True
        for w in weeks:
            avail = {t for t in TEAMS if bye_of[t] != w}
            m = _perfect_matching(avail, games, remaining, week, rng)
            if m is None:
                ok = False
                break
            for gi in m:
                week[gi] = w
                a, b = games[gi]
                remaining[a].remove(gi)
                remaining[b].remove(gi)
        if ok:
            return week
    raise SystemExit("no week assignment")


def _perfect_matching(avail, games, remaining, week, rng, budget=[0]):
    budget[0] = 0

    def rec(free):
        budget[0] += 1
        if budget[0] > 20000:
            return None
        if not free:
            return []
        # most constrained free team
        def opts(t):
            seen, out = set(), []
            for gi in remaining[t]:
                a, b = games[gi]
                o = b if a == t else a
                if o in free and o != t and o not in seen:
                    seen.add(o)
                    out.append(gi)
            return out
        t = min(free, key=lambda x: (len(opts(x)), rng.random()))
        cand = opts(t)
        rng.shuffle(cand)
        for gi in cand:
            a, b = games[gi]
            o = b if a == t else a
            r = rec(free - {t, o})
            if r is not None:
                return [gi] + r
        return None

    return rec(frozenset(avail))


def orient(games, rng):
    """Side A = home. Division pairs: one each way. Singles: Euler-style balancing to 8 home each."""
    out, singles, seen = [], [], Counter()
    for g in games:
        k = frozenset(g)
        seen[k] += 1
    done = Counter()
    for g in games:
        k = frozenset(g)
        if seen[k] == 2:
            a, b = sorted(g)
            out.append((a, b) if done[k] == 0 else (b, a))
            done[k] += 1
        else:
            singles.append(g)
    for _ in range(100000):  # random orientation repair until every team has 8 home
        home = Counter(a for a, b in out)
        cur = [(a, b) if rng.random() < 0.5 else (b, a) for a, b in singles]
        for _ in range(5000):
            h = home + Counter(a for a, b in cur)
            bad = [t for t in TEAMS if h[t] > 8]
            if not bad:
                return out + cur
            t = rng.choice(bad)
            idx = [i for i, (a, b) in enumerate(cur) if a == t]
            i = rng.choice(idx)
            cur[i] = (cur[i][1], cur[i][0])
    raise SystemExit("orientation failed")


def blocks(sched):
    """sched: list of (week, (home, away)). Returns 17 week blocks of 0x64 bytes."""
    out = []
    for w in range(WEEKS):
        gs = [g for wk, g in sched if wk == w]
        playing = {t for g in gs for t in g}
        byes = [t for t in TEAMS if t not in playing]
        b = bytearray(b"\xff" * 0x64)
        for i, (h, a) in enumerate(gs):
            flag = 4 if i >= len(gs) - 2 else 2      # vanilla pattern (meaning unverified)
            if w == 0 and i == 0:
                flag = 1
            b[5 * i:5 * i + 5] = bytes([h[0], h[1], a[0], a[1], flag])
        for i in range(len(gs), 16):
            b[5 * i:5 * i + 5] = b"\xff\xff\xff\xff\x80"
        for k, (d, s) in enumerate(byes):
            b[0x50 + 4 * k:0x54 + 4 * k] = d.to_bytes(2, "big") + s.to_bytes(2, "big")
        out.append(bytes(b))
    return out


def validate(blks):
    games, bye_count = Counter(), Counter()
    home = Counter()
    for w, b in enumerate(blks):
        assert len(b) == 0x64
        seen = set()
        i = 0
        while b[5 * i + 2] != 0xFF:
            h, a = (b[5 * i], b[5 * i + 1]), (b[5 * i + 2], b[5 * i + 3])
            assert h in TEAMS and a in TEAMS and h != a
            assert h not in seen and a not in seen, f"week {w}: team twice"
            seen |= {h, a}
            games[h] += 1
            games[a] += 1
            home[h] += 1
            i += 1
        assert i <= 16
        k = 0
        while int.from_bytes(b[0x50 + 4 * k:0x52 + 4 * k], "big", signed=True) != -1:
            t = (b[0x51 + 4 * k], b[0x53 + 4 * k])
            assert t not in seen
            seen.add(t)
            bye_count[t] += 1
            k += 1
        assert len(seen) == 32, f"week {w}: {len(seen)} teams accounted for"
        assert i + k // 2 * 0 == i and 2 * i + k == 32
        assert 0x50 + 4 * k + 2 <= 0x64
    assert all(games[t] == 16 and bye_count[t] == 1 and home[t] == 8 for t in TEAMS), (games, bye_count, home)
    return True


def main():
    seed = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 2026
    rng = random.Random(seed)
    games = pairings(rng)
    assert len(games) == 256
    games = orient(games, rng)
    week = assign_weeks(games, rng)
    blks = blocks(list(zip(week, games)))
    validate(blks)
    per_week = [sum(1 for w in week if w == k) for k in range(WEEKS)]
    print(f"seed {seed}: 256 games, games per week {per_week}; every team 16 games, 8 home, 1 bye (weeks 3-10)")
    if "--inc" in sys.argv:
        path = sys.argv[sys.argv.index("--inc") + 1]
        with open(path, "w") as f:
            f.write(f"// Generated by tools/season32_schedule.py --seed {seed}: 17 week blocks of 0x64 bytes\n")
            f.write("// (games {divA, slotA, divB, slotB, flags}, byes {s16 div, s16 slot} at +0x50). Don't edit.\n")
            for w, b in enumerate(blks):
                f.write("{" + ",".join(f"0x{x:02X}" for x in b) + "},\n")
        print(f"wrote {path}")
    if "--hex" in sys.argv:
        for w, b in enumerate(blks):
            print(f"week {w:2}: {b.hex()}")


if __name__ == "__main__":
    main()
