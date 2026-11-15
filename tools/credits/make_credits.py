#!/usr/bin/env python3
"""art/credits/credits.png as the game image art/credits/credits.wms (bundled with the app; src/game/engine32.cpp shows
it once per run). The page goes in at 2/3 height over the Players Inc screen's 512x256 frame, which the screen
stretches back to 4:3; its flags and origin come from that screen's own image in the ROM.

  .venv/bin/python tools/credits/make_credits.py [--rom build/baserom.z64]
"""
# BRANDING: the credits page art
import argparse
import struct
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "announcer"))
import fs  # noqa: E402
import wms  # noqa: E402

PAGE = ROOT / "art" / "credits" / "credits.png"
OUT = ROOT / "art" / "credits" / "credits.wms"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    a = ap.parse_args()
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    ref = fs.read(rom, fs.by_name(ents)["nflpalg.wms"])
    _, w, h, ox, oy = struct.unpack_from(">IIIii", ref, 4)
    img = Image.open(PAGE).convert("RGBA").resize((w, h), Image.LANCZOS)
    OUT.write_bytes(wms.encode_rgba16(img, ref[3], ox, oy))
    print(f"wrote {OUT.relative_to(ROOT)} ({w}x{h})")


if __name__ == "__main__":
    main()
