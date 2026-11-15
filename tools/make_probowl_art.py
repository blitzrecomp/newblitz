#!/usr/bin/env python3
"""Pro Bowl art for Modern Blitz: our own team-select-sized logo, small logo and win wordmark for a Pro Bowl crew,
in the game's formats (each matched to a base team's image of that role, read from your ROM), one colourway per side.

  .venv/bin/python tools/make_probowl_art.py --app-folder <app folder> [--rom build/teamplus.z64]

Writes <app folder>/probowl/pb_<lg|ls|win>_<l|r>.wms (registered as added game files at startup, crew.cpp) and a review
sheet at build/analysis/probowl_art.png. The drawings are ours (a star, "PRO BOWL" lettering); only the image
headers' sizes and origins come from the ROM, so the output stays out of git.
"""
import argparse
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "announcer"))
import fs  # noqa: E402
import wms  # noqa: E402
from make_league_pak import base_files, fit, lettering, rgb, shadow, wordmark  # noqa: E402

SIDES = {"l": ("#c8102e", "#0b1f4a"), "r": ("#1450b4", "#0b1f4a")}  # star, outline


def star(cx, cy, r_out, r_in, rot=-90):
    import math
    pts = []
    for i in range(10):
        r = r_out if i % 2 == 0 else r_in
        a = math.radians(rot + i * 36)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def logo(w, h, fill, outline, words=True):
    """A white-edged star in the side's colour, with PRO / BOWL lettering across it."""
    S = 4
    W, H = w * S, h * S
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = min(W, H) * 0.48
    cx, cy = W / 2, H * 0.52
    d.polygon(star(cx, cy, r, r * 0.45), fill=rgb(outline))
    d.polygon(star(cx, cy, r * 0.90, r * 0.40), fill=(255, 255, 255, 255))
    d.polygon(star(cx, cy, r * 0.80, r * 0.35), fill=rgb(fill))
    if words:
        top = lettering("PRO", (255, 255, 255, 255), rgb(outline), None, height=int(H * 0.30), stroke=int(H * 0.025))
        img.alpha_composite(fit(top, int(W * 0.46), int(H * 0.28)), (int(W * 0.27), int(H * 0.30)))
        bot = lettering("BOWL", (255, 236, 40, 255), rgb(outline), None, height=int(H * 0.28), stroke=int(H * 0.025))
        img.alpha_composite(fit(bot, int(W * 0.62), int(H * 0.26)), (int(W * 0.19), int(H * 0.58)))
    img = img.resize((w, h), Image.LANCZOS)
    return shadow(img, (2, 2), 1.2, 140)


def encode_ci8(img, flags, ox, oy):
    """Type 0: 8-bit colour index and a 256-colour RGBA5551 palette (index 0 transparent)."""
    w, h = img.size
    rgb_img = Image.new("RGB", img.size, (0, 0, 0))
    rgb_img.paste(img, mask=img.getchannel("A"))
    q = rgb_img.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
    pal = q.getpalette()[: 255 * 3]
    alpha = img.getchannel("A").load()
    qpx = q.load()
    px = bytes(0 if alpha[x, y] < 128 else qpx[x, y] + 1 for y in range(h) for x in range(w))
    px += bytes((-len(px)) % 8)
    colours = [0] + [wms.to5551(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2], 255) for i in range(len(pal) // 3)]
    colours += [0] * (256 - len(colours))
    return b"wn\x02" + bytes([flags]) + struct.pack(">IIIii", 0, w, h, ox, oy) + bytes(8) + px + b"".join(struct.pack(">H", c) for c in colours)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=str(ROOT / "build" / "teamplus.z64"))
    ap.add_argument("--app-folder", required=True)
    ap.add_argument("--sheet", default=str(ROOT / "build" / "analysis" / "probowl_art.png"))
    a = ap.parse_args(argv)
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    bf = base_files(rom, ents, "ARI")
    out = Path(a.app_folder) / "probowl"
    out.mkdir(parents=True, exist_ok=True)
    sheet = []
    for side, (fill, outline) in SIDES.items():
        art = {
            "logo": logo(bf["logo"]["w"], bf["logo"]["h"], fill, outline),
            "logo_small": logo(bf["logo_small"]["w"], bf["logo_small"]["h"], fill, outline, words=False),
            "win": wordmark("PRO BOWL", rgb(fill), rgb("#101018"), bf["win"]["w"], bf["win"]["h"]),
        }
        for role, img in art.items():
            info = bf[role]
            enc = encode_ci8 if info["type"] == 0 else wms.encode_type19
            short = {"logo": "lg", "logo_small": "ls", "win": "win"}[role]  # game file names: 15 characters at most
            (out / f"pb_{short}_{side}.wms").write_bytes(enc(img, info["flags"] or 4, *info["origin"]))
            sheet.append(img)
    W = sum(i.width + 8 for i in sheet) + 8
    H = max(i.height for i in sheet) + 16
    s = Image.new("RGBA", (W, H), (60, 64, 80, 255))
    x = 8
    for i in sheet:
        s.alpha_composite(i, (x, 8))
        x += i.width + 8
    Path(a.sheet).parent.mkdir(parents=True, exist_ok=True)
    s.save(a.sheet)
    print(f"{len(sheet)} images in {out}; review sheet {a.sheet}")
    for role in ("logo", "logo_small", "win"):
        print(f"  {role}: {bf[role]['w']}x{bf[role]['h']} type {bf[role]['type']}")


if __name__ == "__main__":
    main()
