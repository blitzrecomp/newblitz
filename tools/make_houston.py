#!/usr/bin/env python3
"""Houston for Modern on the original game (src/game/engine32.cpp), from our own sources only: no Team Extension.

  .venv/bin/python tools/make_houston.py [--rom build/baserom.z64] [--art build/team_art_out]
                                         [--app-folder build/appdata2026] [--out DIR ...]

Writes <out>/houston.json and Houston's game files (hou_*.wms) into each --out (default: build/engine32/houston and
<app folder>/houston). engine32 builds Houston's record from them:
  - the base: the Titans' record from your ROM (the Oilers' line, Houston's division rival): its CPU play lists, CPU
    skill, selector, VS pause and player sizes. Ratings and the 15 players come from the content pack (and the weekly
    lineups), as for every team.
  - names: HOUSTON / Texans / HOU.
  - art (art round 5, build/team_art_out/HOU): logos, wordmark, winner text, VS helmets both ways, end zones, and the
    HOUSTON city banner drawn by tools/make_vs_banners.py's renderer.
  - uniforms: our 2026 home and away sets (<app folder>/uniforms/HOU/00_home_2026, 01_away_2026).
  - jersey clashes: the visitor changes to white in every Houston game but Dallas's (white at home), as the game's own
    table has it for the league (only the Cowboys' row is all "no change").
Placement (origins) comes from the Titans' own files, scaled to our canvases. Everything written is personal-use art:
build/ and app folders only, never git.
"""
import argparse
import json
import shutil
import struct
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "announcer"))
import fs  # noqa: E402
import make_vs_banners  # noqa: E402
import wms  # noqa: E402
from import_team_art import PARTS, encode, header  # noqa: E402

BASE = "TEN"
# Each piece: our file name, and the Titans' file whose placement it takes.
PIECES = {"logo": ("hou_logo.wms", "l_titans.wms"), "small_logo": ("hou_slogo.wms", "l_tenn30.wms"),
          "title_nick": ("hou_nick.wms", "ti_titans.wms"), "win": ("hou_win.wms", "win_tit.wms"),
          "vs_left": ("hou_vsl.wms", "tentitlf.wms"), "vs_right": ("hou_vsr.wms", "tentitlf.wms"),
          "end_zone_1": ("hou_ez1.wms", "tit_ez1.wms"), "end_zone_2": ("hou_ez2.wms", "tit_ez2.wms"),
          "end_zone_3": ("hou_ez3.wms", "tit_ez3.wms")}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    ap.add_argument("--art", default=str(ROOT / "build" / "team_art_out"))
    ap.add_argument("--app-folder", default=str(ROOT / "build" / "appdata2026"))
    ap.add_argument("--out", action="append")
    a = ap.parse_args()
    app = Path(a.app_folder)
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    by_name = fs.by_name(ents)
    files = {}
    for piece, (name, ref_name) in PIECES.items():
        img = Image.open(Path(a.art) / "HOU" / f"{piece}.png").convert("RGBA")
        files[name] = encode(piece, img, header(fs.read(rom, by_name[ref_name])))
    city = header(fs.read(rom, by_name["ti_ten.wms"]))
    base, _ = make_vs_banners.plate(rom, ents)
    files["hou_city.wms"] = wms.encode_rgba16(make_vs_banners.banner(base, "HOUSTON"), city["flags"], *city["origin"])
    uniform = {}
    for side, d in (("home", "00_home_2026"), ("away", "01_away_2026")):
        src = app / "uniforms" / "HOU" / d
        names = []
        for k, part in enumerate(PARTS):
            f = src / f"{part}.wms"
            if not f.exists():
                raise SystemExit(f"{f} is missing: Houston's base uniforms need all ten parts")
            name = f"hou_{side[0]}{k}.wms"
            files[name] = f.read_bytes()
            names.append(name)
        uniform[side] = names
    record = {"base": BASE, "franchise": "HOU", "city": "Houston", "nickname": "Texans", "select": "Houston",
              "abbr": "HOU", "prefix": "TEX", "no_clash": ["DAL"],
              "art": {piece: name for piece, (name, _) in PIECES.items()} | {"title_city": "hou_city.wms"},
              "uniform": uniform}
    outs = [Path(p) for p in (a.out or [ROOT / "build" / "engine32" / "houston", app / "houston"])]
    for out in outs:
        if out.exists():
            shutil.rmtree(out)  # the Team Extension's block, clash row and files go with it
        out.mkdir(parents=True)
        for name, data in files.items():
            (out / name).write_bytes(data)
        (out / "houston.json").write_text(json.dumps(record, indent=1))
        print(f"{out}: houston.json and {len(files)} files")


if __name__ == "__main__":
    main()
