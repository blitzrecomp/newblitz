#!/usr/bin/env python3
"""Headshots for every rostered player, for the app to carry (Pick Your Crew on a console or an offline phone):
ESPN's 96x70 JPEGs, the same request src/game/crew.cpp makes, into build/roster_faces/<ESPN id>.jpg. Only missing
ones are fetched, a few at a time; a player ESPN has no picture for gets an empty marker so he isn't asked again.
`make` copies the folder into each Modern build's rosters/faces (the Makefile's roster_snapshot).

  .venv/bin/python tools/roster_headshots.py [--players build/blitz-rosters/players.json] [--out build/roster_faces]

ESPN's images are for the team's private builds only (docs/PROVENANCE.md); build/ is never committed.
"""
import argparse
import concurrent.futures
import json
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
URL = "https://a.espncdn.com/combiner/i?img=/i/headshots/nfl/players/full/{id}.png&w=96&h=70&format=jpg"


def fetch(pid, out):
    path, miss = out / f"{pid}.jpg", out / f"{pid}.none"
    if path.exists() or miss.exists():
        return "kept"
    req = urllib.request.Request(URL.format(id=pid), headers={"User-Agent": "BlitzRecomp"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                body = r.read()
            if len(body) > 100:
                tmp = path.with_suffix(".part")
                tmp.write_bytes(body)
                tmp.rename(path)
                return "new"
            miss.touch()
            return "none"
        except urllib.error.HTTPError as e:
            if e.code == 404:
                miss.touch()
                return "none"
            time.sleep(1 + attempt)
        except Exception:
            time.sleep(1 + attempt)
    return "failed"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--players", default=str(ROOT / "build" / "blitz-rosters" / "players.json"))
    ap.add_argument("--out", default=str(ROOT / "build" / "roster_faces"))
    ap.add_argument("--workers", type=int, default=8)
    a = ap.parse_args()
    players = json.loads(Path(a.players).read_text())["players"]
    ids = [pid for pid, p in players.items() if p.get("team") and pid.isdigit()]
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    counts = {}
    with concurrent.futures.ThreadPoolExecutor(a.workers) as pool:
        for result in pool.map(lambda pid: fetch(pid, out), ids):
            counts[result] = counts.get(result, 0) + 1
    size = sum(f.stat().st_size for f in out.glob("*.jpg"))
    print(f"{len(ids)} players: " + ", ".join(f"{v} {k}" for k, v in sorted(counts.items())) +
          f"; {len(list(out.glob('*.jpg')))} faces, {size / 1e6:.1f} MB in {out}")
    return 1 if counts.get("failed") else 0


if __name__ == "__main__":
    sys.exit(main())
