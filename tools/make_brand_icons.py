#!/usr/bin/env python3
"""The New Blitz 2027 app icon and launcher logo, from the design sources in icons/source/:

  icons/app_2026.png    1024x1024, from app_newblitz_2027.png (the app icon on every platform)
  icons/logo_2026.png   1400 wide, from logo_newblitz_2027.png trimmed to its solid edge (launcher, PS5 art)

  .venv/bin/python tools/make_brand_icons.py

Then tools/make_logo2027.py and --overlays for the in-game copies. Needs Pillow (.venv).
"""
# BRANDING: the app icon and launcher logo
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "icons" / "source"
OUT = ROOT / "icons"


def main():
    icon = Image.open(SRC / "app_newblitz_2027.png").convert("RGBA")
    icon.resize((1024, 1024), Image.LANCZOS).save(OUT / "app_2026.png", optimize=True)

    logo = Image.open(SRC / "logo_newblitz_2027.png").convert("RGBA")
    box = logo.getchannel("A").point(lambda v: 255 if v >= 32 else 0).getbbox()  # the flame's faint glow is left out
    pad = 2
    box = (max(0, box[0] - pad), max(0, box[1] - pad), min(logo.width, box[2] + pad), min(logo.height, box[3] + pad))
    logo = logo.crop(box)
    logo.resize((1400, round(1400 * logo.height / logo.width)), Image.LANCZOS).save(OUT / "logo_2026.png", optimize=True)
    print("icons/app_2026.png, icons/logo_2026.png")


if __name__ == "__main__":
    main()
