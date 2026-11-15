#!/usr/bin/env python3
"""Turn the Blitz '27 PNG kit (docs/design/redesign/ui/, drawn by tools/make_ui_assets.py) into what the app's
UI (recompui) can draw: it has no 9-slice or tiling, so frames are cut into pieces here (the grid tile is repeated
by an RmlUi image decorator). Writes assets/ui/ (bundled with the app; src/main/ui_kit.cpp loads it).

  .venv/bin/python tools/ui_kit_assets.py

Uses the @2x files (sharp to 1440p). Pieces are named <frame>.<part>.png, part in tl t tr l c r bl b br
(9-slice) or t c b (vertical 3-slice). Insets are in 1x px, the size recompui lays out in.
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "design" / "redesign" / "ui"
OUT = ROOT / "assets" / "ui"
SCALE = 2

NINE = {  # name: (top, right, bottom, left) insets at 1x
    "panel_frame": (16, 16, 16, 16),
    "panel_frame_p1": (16, 16, 16, 16),
    "panel_frame_p2": (16, 16, 16, 16),
    "panel_frame_p3": (16, 16, 16, 16),
    "panel_frame_p4": (16, 16, 16, 16),
    "header_bevel": (20, 24, 20, 24),
}
THREE_V = {"selector_cap_l": (14, 14), "selector_cap_r": (14, 14)}  # top, bottom
WHOLE = ["bg_title", "bg_overlay_swoosh", "chrome_corner_tl", "chrome_corner_tr", "chrome_corner_bl", "chrome_corner_br",
         "seg_on", "seg_off", "grid_tile", "rule_flame", "rule_chrome", "rule_chrome_rev", "logo27_lockup"]  # panel_frame_hot and flame_strip: nothing draws them (review P6)


def src(name):
    p = SRC / f"{name}@2x.png"
    return Image.open(p if p.exists() else SRC / f"{name}.png").convert("RGBA")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.png"):
        old.unlink()
    for name in WHOLE:
        src(name).save(OUT / f"{name}.png", optimize=True)
    for name, (t, r, b, l) in NINE.items():
        im = src(name)
        W, H = im.size
        t, r, b, l = (v * SCALE for v in (t, r, b, l))
        xs, ys = [0, l, W - r, W], [0, t, H - b, H]
        parts = [["tl", "t", "tr"], ["l", "c", "r"], ["bl", "b", "br"]]
        for row in range(3):
            for col in range(3):
                im.crop((xs[col], ys[row], xs[col + 1], ys[row + 1])).save(OUT / f"{name}.{parts[row][col]}.png", optimize=True)
    for name, (t, b) in THREE_V.items():
        im = src(name)
        W, H = im.size
        t, b = t * SCALE, b * SCALE
        for part, box in (("t", (0, 0, W, t)), ("c", (0, t, W, H - b)), ("b", (0, H - b, W, H))):
            im.crop(box).save(OUT / f"{name}.{part}.png", optimize=True)
    print(f"{len(list(OUT.glob('*.png')))} files in {OUT}")


if __name__ == "__main__":
    main()
