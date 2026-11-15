#!/usr/bin/env python3
"""VS screen city banners for the 2026 cities: LAS VEGAS (Raiders) and LOS ANGELES (Chargers, Rams).

  .venv/bin/python tools/make_vs_banners.py [--rom ROM] [--app-folder DIR] [--preview DIR]

The VS screen draws each team's city as an image (team block +0x1DC: ti_oak.wms, ti_sand.wms,
ti_stl.wms), so the moved teams still read OAKLAND, SAN DIEGO and ST LOUIS. This makes new ones that
sit next to the opponent's original: the banner plate (chrome brackets, black panel, teal grid) comes
from your ROM's Miami banner with its text painted over by the grid, and the lettering is drawn here
(Arial Black stretched and slanted, yellow with a black keyline and shadow).

Outputs go to <app folder>/art/files/ (default build/appdata2026, the dev app), where the art drop-in
loads them in place of the originals at the next start; --preview also writes PNGs. They contain the
ROM's plate, so they stay out of git like everything else derived from the game.
"""
import argparse
import os
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "announcer"))
import fs  # noqa: E402
import wms  # noqa: E402

S = 8  # supersampling
FONT = "/System/Library/Fonts/Supplemental/Arial Black.ttf"
BANNERS = {"ti_oak.wms": "LAS VEGAS", "ti_sand.wms": "LOS ANGELES", "ti_stl.wms": "LOS ANGELES"}
PLATE_FROM = "ti_mia.wms"  # the shortest city text (x 79-176), so the least to paint over


def plate(rom, ents):
    """The Miami banner with its text replaced by the panel's own grid (repeats every 15 px)."""
    img, info = wms.decode(fs.read(rom, fs.by_name(ents)[PLATE_FROM]))
    px, out = img.load(), img.copy()
    po = out.load()
    for y in range(8, 23):
        for x in range(70, 186):
            sx = x
            while sx >= 70:
                sx -= 15
            po[x, y] = px[sx, y]
    return out, info


def lettering(text, cap=12, stretch=1.5, shear=0.3, max_w=214):
    """The text as an 8x supersampled mask: caps `cap` px tall, stretched and slanted like the game's."""
    f = ImageFont.truetype(FONT, 40 * S)
    hb = f.getbbox("H")
    big = Image.new("L", (4000, 600), 0)
    ImageDraw.Draw(big).text((100, 100), text, font=f, fill=255)
    big = big.crop(big.getbbox())
    scale = cap * S / (hb[3] - hb[1])
    w, h = int(big.width * scale * stretch), int(big.height * scale)
    big = big.resize((w, h), Image.LANCZOS)
    big = big.transform((w + int(shear * h) + 2, h), Image.AFFINE, (1, shear, -shear * h, 0, 1, 0), Image.BICUBIC)
    big = big.crop(big.getbbox())
    if big.width > max_w * S:
        big = big.resize((max_w * S, big.height), Image.LANCZOS)
    return big


def banner(base, text):
    w, h = base.size
    mask = Image.new("L", (w * S, h * S), 0)
    m = lettering(text)
    mask.paste(m, (int(127.5 * S - m.width / 2), 9 * S))
    t = mask.resize((w, h), Image.LANCZOS)
    halo = ImageChops.lighter(t.filter(ImageFilter.MaxFilter(3)), ImageChops.offset(t, 1, 1))  # keyline + shadow
    out = base.copy()
    po, hp, tp = out.load(), halo.load(), t.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = po[x, y]
            k = hp[x, y] / 255
            if k and a:
                r, g, b = (int(c * (1 - k)) for c in (r, g, b))
            v = tp[x, y] / 255
            if v:
                r, g, b, a = int(r + (255 - r) * v), int(g + (255 - g) * v), int(b * (1 - v)), 255
            po[x, y] = (r, g, b, a)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    ap.add_argument("--app-folder", default=str(ROOT / "build" / "appdata2026"))
    ap.add_argument("--preview", help="also write PNG previews here")
    a = ap.parse_args(argv)
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    if PLATE_FROM not in fs.by_name(ents):
        raise SystemExit(f"{a.rom} has no {PLATE_FROM}: not an NFL Blitz 2000 ROM with the game's art")
    base, info = plate(rom, ents)
    out_dir = Path(a.app_folder) / "art" / "files"
    out_dir.mkdir(parents=True, exist_ok=True)
    made = {}
    for name, text in BANNERS.items():
        img = made.get(text) or banner(base, text)
        made[text] = img
        (out_dir / name).write_bytes(wms.encode_rgba16(img, info["flags"], *info["origin"]))
        if a.preview:
            os.makedirs(a.preview, exist_ok=True)
            img.save(Path(a.preview) / name.replace(".wms", ".png"))
        print(f"{name}: {text}")
    print(f"wrote {len(BANNERS)} banners to {out_dir}; they load at the app's next start")


if __name__ == "__main__":
    main()
