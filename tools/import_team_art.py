#!/usr/bin/env python3
"""Our own 2D art for the Commanders (art round 5, build/team_art_out) and every team (round 6, --round6) into the
game's formats. Houston is built by tools/make_houston.py from the same round's art.

  .venv/bin/python tools/import_team_art.py [--art build/team_art_out] [--app-folder build/appdata2026]
                                            [--rom build/baserom.z64] [--round6 --art build/team_art_out_r6]

The Commanders: the pieces are encoded under Washington's game file names into <app folder>/art/files (the art
drop-in, Modern only: Classic keeps the 1999 art). Files there from an earlier borrow are moved to
build/borrowed_art_backup first.

Everything written is personal-use art: it stays in build/ and app folders, never in git.
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
import wms  # noqa: E402
from make_league_pak import encode_ci4_exact  # noqa: E402

PARTS = ["helmet_side", "helmet_stripe_front", "helmet_stripe_back", "facemask", "shoulder", "jersey", "pant_stripe",
         "pants", "socks", "numbers"]
WAS_FILES = {"logo": "l_redskins.wms", "small_logo": "l_wash30.wms", "title_nick": "ti_redskins.wms",
             "win": "win_red.wms", "vs_left": "wasredlf.wms", "end_zone_1": "red_ez1.wms", "end_zone_2": "red_ez2.wms",
             "end_zone_3": "red_ez3.wms"}


def header(data):
    _, w, h, ox, oy = struct.unpack_from(">IIIii", data, 4)
    return {"w": w, "h": h, "origin": (ox, oy), "flags": data[3]}


def encode(piece, img, was):
    """The piece in the format it needs, its origin from the file it replaces (scaled when the canvas changed)."""
    w, h = img.size
    ox, oy = was["origin"]
    if (w, h) != (was["w"], was["h"]):
        if piece == "win":
            ox = w // 2  # the game's winner words are centred: origin x = width / 2
        else:
            ox, oy = round(ox * w / was["w"]), round(oy * h / was["h"])
    if piece.startswith("end_zone"):
        # The game's own strips are stored mirrored (pan_ez1/2, ram_ez1/2 read backwards however they're turned; the
        # field draws them flipped). The art rounds deliver readable strips, so each is mirrored left to right here.
        if piece in ("end_zone_1", "end_zone_2"):
            img = img.transpose(Image.FLIP_LEFT_RIGHT)
        return encode_ci4_exact(img, flags=was["flags"], ox=ox, oy=oy)
    if piece == "small_logo":
        return wms.encode_rgba16(img, was["flags"], ox, oy)  # hard alpha
    return wms.encode_type19(img, was["flags"], ox, oy)  # soft edges


def encode_auto(piece, img, was):
    """Round 6: end zones in 16 colours, the scoreboard logo with hard alpha, the rest by their alpha."""
    if piece.startswith("end_zone") or piece == "small_logo":
        return encode(piece, img, was)
    px = img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()
    soft = any(0 < p[3] < 255 for p in px)
    w, h = img.size
    ox, oy = was["origin"]
    if (w, h) != (was["w"], was["h"]):
        ox, oy = round(ox * w / was["w"]), round(oy * h / was["h"])
    return (wms.encode_type19 if soft else wms.encode_rgba16)(img, was["flags"], ox, oy)


def round6(art, kit, app, rom, ents, by_name):
    """Every team's redone pieces (by game file name), right-side VS helmets (per team, <app>/art/vs_right), the
    league's marks, and Washington's gold-facemask helmet: Modern's art drop-in only."""
    manifest = json.loads((kit / "manifest_round6.json").read_text())
    files, vs_right = app / "art" / "files", app / "art" / "vs_right"
    files.mkdir(parents=True, exist_ok=True)
    vs_right.mkdir(parents=True, exist_ok=True)
    n = 0
    for team_dir in sorted(p for p in art.iterdir() if p.is_dir() and p.name.isupper()):
        abbr = team_dir.name
        pieces = manifest["teams"].get(abbr) or {k: {"game_file": v} for k, v in WAS_FILES.items()} if abbr == "WAS" else manifest["teams"].get(abbr)
        if pieces is None:
            print(f"{abbr}: not in the kit's manifest; skipped")
            continue
        for png in sorted(team_dir.glob("*.png")):
            piece = png.stem
            img = Image.open(png).convert("RGBA")
            if piece == "vs_right":
                ref = header(fs.read(rom, by_name[pieces["vs_left"]["game_file"]]))
                (vs_right / f"{abbr}.wms").write_bytes(encode_auto(piece, img, ref))
            else:
                name = pieces[piece]["game_file"]
                (files / name).write_bytes(encode_auto(piece, img, header(fs.read(rom, by_name[name]))))
            n += 1
    for png in sorted((art / "league").glob("*.png")):
        name = png.stem + ".wms"
        img = Image.open(png).convert("RGBA")
        (files / name).write_bytes(encode_auto(png.stem, img, header(fs.read(rom, by_name[name]))))
        n += 1
    # The credits page isn't imported any more: it's in git (tools/credits/, art/credits/) and every build bundles it.
    print(f"{n} files into {files}, {vs_right}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--art", default=str(ROOT / "build" / "team_art_out"))
    ap.add_argument("--app-folder", default=str(ROOT / "build" / "appdata2026"))
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    ap.add_argument("--round6", action="store_true", help="art round 6 (build/team_art_out_r6): every team and the league")
    ap.add_argument("--kit", default=str(ROOT / "build" / "team_art_kit_r6"))
    a = ap.parse_args()
    art, app = Path(a.art), Path(a.app_folder)
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    by_name = fs.by_name(ents)
    if a.round6:
        return round6(art, Path(a.kit), app, rom, ents, by_name)

    # ---- The Commanders ----
    files = app / "art" / "files"
    files.mkdir(parents=True, exist_ok=True)
    backup = ROOT / "build" / "borrowed_art_backup"
    for piece, name in WAS_FILES.items():
        target = files / name
        if target.exists() and not (backup / name).exists():
            backup.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, backup / name)
        ref = header(fs.read(rom, by_name[name]))
        if piece in ("logo", "small_logo") and target.exists():
            ref = header(target.read_bytes())  # the borrowed canvas, already placed in game: same size as ours
        target.write_bytes(encode(piece, Image.open(art / "WAS" / f"{piece}.png").convert("RGBA"), ref))
    print(f"{files}: {len(WAS_FILES)} Commanders files (earlier ones kept in {backup})")
    (art / "IMPORTED.json").write_text(json.dumps({"commanders": str(files)}, indent=1))


if __name__ == "__main__":
    main()
