#!/usr/bin/env python3
"""Player headshots for the Roster Room page (a skin-tone reference while editing): matches each pack player
to ESPN by team, number and last name, downloads ESPN's headshot, and packs them into one sprite sheet.

  .venv/bin/python tools/headshots.py [--pack packs/blitz2026] [--out build/headshots]

Writes build/headshots/sheet.jpg (each face 60x44 at 2x, one row per team in pack order) and map.json
({franchise: {slot: [col, row]}}, plus "missing"). ESPN's images are only fetched for this private page;
nothing here is committed (build/ is ignored).
"""
import argparse
import io
import json
import sys
import time
import urllib.request
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import roster_fetch  # noqa: E402

W, H = 120, 88  # 2x of the 60x44 shown
URL = "https://a.espncdn.com/combiner/i?img=/i/headshots/nfl/players/full/{id}.png&w=240&h=175"


_nfl_urls = None


def nfl_headshot_url(aid):
    """A player keyed nfl:<gsis id> (no ESPN id: roster_nflverse.py) has no ESPN photo; nflverse's roster file names
    the league's own headshot. For the team's private Roster Room only, as the ESPN ones are."""
    global _nfl_urls
    if _nfl_urls is None:
        import csv, datetime
        year = datetime.date.today().year if datetime.date.today().month >= 3 else datetime.date.today().year - 1
        url = f"https://github.com/nflverse/nflverse-data/releases/download/rosters/roster_{year}.csv"
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "BlitzRecomp roster room"}), timeout=60) as r:
                rows = csv.DictReader(io.StringIO(r.read().decode("utf-8")))
                _nfl_urls = {row["gsis_id"]: row["headshot_url"] for row in rows if row.get("headshot_url")}
        except Exception:
            _nfl_urls = {}
    return _nfl_urls.get(aid[4:])


def fetch_image(aid):
    if str(aid).startswith("nfl:"):
        url = nfl_headshot_url(aid)
        if not url:
            return None
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "BlitzRecomp roster room"}), timeout=20) as r:
                return Image.open(io.BytesIO(r.read())).convert("RGBA")
        except Exception:
            return None
    for i in range(3):
        try:
            req = urllib.request.Request(URL.format(id=aid), headers={"User-Agent": "BlitzRecomp roster room"})
            with urllib.request.urlopen(req, timeout=20) as r:
                return Image.open(io.BytesIO(r.read())).convert("RGBA")
        except Exception:
            time.sleep(1.0)
    return None


def norm(s):
    return "".join(c for c in roster_fetch.clean_last(s).upper() if c.isalpha())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", default=str(ROOT / "packs" / "blitz2026"))
    ap.add_argument("--out", default=str(ROOT / "build" / "headshots"))
    a = ap.parse_args()
    pack = json.load(open(Path(a.pack) / "pack.json"))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    ids = roster_fetch.team_ids()
    teams = pack["teams"]
    sheet = Image.new("RGB", (W * 15, H * len(teams)), (13, 26, 58))
    mapping, missing = {}, []
    for row, t in enumerate(teams):
        f = t["franchise"]
        espn = roster_fetch.roster(ids[f]) if f in ids else {}
        by_num = {}
        for p in espn.values():
            by_num.setdefault(p["number"], []).append(p)
        mapping[f] = {}
        for p in t.get("players", []):
            slot, last, num = p["slot"], p.get("last", ""), p.get("number")
            cands = [c for c in by_num.get(num, []) if norm(c["last"]) == norm(last)]
            if not cands:  # same name, different number (a mid-season number change)
                cands = [c for c in espn.values() if norm(c["last"]) == norm(last)]
            if len(cands) != 1:
                missing.append(f"{f} {slot} {num} {last}")
                continue
            img = fetch_image(cands[0]["id"])
            if img is None:
                missing.append(f"{f} {slot} {num} {last} (no image)")
                continue
            # Fill the cell, cropped from the top (faces sit in the upper part of ESPN's frame).
            k = max(W / img.width, H / img.height)
            img = img.resize((round(img.width * k), round(img.height * k)), Image.LANCZOS)
            cell = Image.new("RGBA", (W, H), (13, 26, 58, 255))
            cell.alpha_composite(img, ((W - img.width) // 2, 0))
            sheet.paste(cell.convert("RGB"), (slot * W, row * H))
            mapping[f][str(slot)] = [slot, row]
        print(f"{f}: {len(mapping[f])}/{len(t.get('players', []))}", flush=True)
    sheet.save(out / "sheet.jpg", quality=72, optimize=True, progressive=True)
    json.dump({"cell": [W // 2, H // 2], "teams": mapping, "missing": missing}, open(out / "map.json", "w"), indent=1)
    print(f"sheet {sheet.size}, {sum(len(v) for v in mapping.values())} faces, {len(missing)} missing")


if __name__ == "__main__":
    main()
