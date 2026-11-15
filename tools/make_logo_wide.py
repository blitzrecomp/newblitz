#!/usr/bin/env python3
"""the team's short, wide Blitz '27 logo (icons/source/lockup_27.png) as the web pages' header logo:
icons/logo_wide_2026.png, 144 px tall (a 48 px header at 3x), palette PNG so it stays small enough to inline.
Used by the phone controller page in Modern Blitz (served as /logo.png, src/main/phone_pads.cpp), Roster Room
and the sit-down checklist (inlined as a data URI).

  .venv/bin/python tools/make_logo_wide.py [--data-uri]
"""
# BRANDING: draws the wide Blitz '27 logo
import base64
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "icons" / "source" / "lockup_27.png"
OUT = ROOT / "icons" / "logo_wide_2026.png"
HEIGHT = 144


def main():
    im = Image.open(SRC).convert("RGBA")
    x0, y0, x1, y1 = im.getchannel("A").point(lambda a: 255 if a > 24 else 0).getbbox()
    pad = 6
    im = im.crop((max(0, x0 - pad), max(0, y0 - pad), min(im.width, x1 + pad), min(im.height, y1 + pad)))
    im = im.resize((round(im.width * HEIGHT / im.height), HEIGHT), Image.LANCZOS)
    im.quantize(256, method=Image.Quantize.FASTOCTREE).save(OUT, optimize=True)
    if "--data-uri" in sys.argv:
        print("data:image/png;base64," + base64.b64encode(OUT.read_bytes()).decode())
    else:
        print(f"wrote {OUT} ({im.width}x{im.height}, {OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
