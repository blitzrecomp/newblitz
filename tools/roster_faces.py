#!/usr/bin/env python3
"""New faces for Roster Room: players in the weekly-roster player file who still need a skin shade, with
ESPN headshots for the team to pick from (nothing is guessed from names or photos).

  .venv/bin/python tools/roster_faces.py [--dir build/blitz-rosters] [--out build/rosters/new_faces.json]

Lists, in order: players in any lineup (this week's or full strength), then each club's depth-chart starters,
then backup quarterbacks, backs and receivers (QB2, RB2, WR2, TE2). The output is the page's "newfaces"
block: {"sheet": jpeg data URI, "cell": [w, h], "faces": [{id, team, first, last, number, pos, depth, group,
at: [col, row]}]}. Shades picked on the page land in its db collection `shades` (one doc per ESPN id);
`roster_season.py shades <that folder>` writes them into players.json.
"""
import argparse
import base64
import io
import json
import re
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from headshots import H, W, fetch_image  # noqa: E402

COLS = 20
NOT_PLAYING = {"kr", "pr", "h", "ls", "p"}  # returners, holders, snappers, punters


def rank(e):
    m = re.match(r"([a-z]+)(\d+)$", e.get("depth") or "")
    return (m.group(1), int(m.group(2))) if m else (None, 99)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=str(ROOT / "build" / "blitz-rosters"))
    ap.add_argument("--out", default=str(ROOT / "build" / "rosters" / "new_faces.json"))
    a = ap.parse_args(argv)
    d = Path(a.dir)
    players = json.loads((d / "players.json").read_text())["players"]
    slotted = set()
    for p in (d / "lineups").glob("*.json"):
        for ids in json.loads(p.read_text())["teams"].values():
            slotted.update(i for i in ids if i)
    faces = []
    for pid, e in players.items():
        if e.get("skin") is not None or not e.get("team"):
            continue
        key, n = rank(e)
        group = ("lineup" if pid in slotted else "starter" if n == 1 and key not in NOT_PLAYING
                 else "backup" if n == 2 and key in ("qb", "rb", "wr", "te") else None)
        if group:
            faces.append(dict(id=pid, team=e["team"], first=e["first"], last=e["last"], number=e["number"],
                              pos=e["pos"], depth=e.get("depth"), group=group))
    order = {"lineup": 0, "starter": 1, "backup": 2}
    faces.sort(key=lambda f: (order[f["group"]], f["team"], f["last"]))

    rows = (len(faces) + COLS - 1) // COLS
    sheet = Image.new("RGB", (W * COLS, H * max(rows, 1)), (13, 26, 58))
    for i, f in enumerate(faces):
        img = fetch_image(f["id"])
        if img is None:
            continue
        k = max(W / img.width, H / img.height)
        img = img.resize((round(img.width * k), round(img.height * k)), Image.LANCZOS)
        cell = Image.new("RGBA", (W, H), (13, 26, 58, 255))
        cell.alpha_composite(img, ((W - img.width) // 2, 0))
        sheet.paste(cell.convert("RGB"), ((i % COLS) * W, (i // COLS) * H))
        f["at"] = [i % COLS, i // COLS]
    buf = io.BytesIO()
    sheet.save(buf, "JPEG", quality=72, optimize=True, progressive=True)
    out = {"sheet": "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode(), "cell": [W // 2, H // 2],
           "cols": COLS, "faces": faces}
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text(json.dumps(out, separators=(",", ":")))
    print(f"{len(faces)} new faces ({sum(f['group'] == 'lineup' for f in faces)} in lineups), "
          f"{sum('at' in f for f in faces)} photos, {len(buf.getvalue()) // 1024} KB -> {a.out}")


if __name__ == "__main__":
    main()
