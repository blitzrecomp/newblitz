#!/usr/bin/env python3
"""New Blitz 2027's own marks where the 1999 game shows the NFL shield (2026-11-14): midfield on its five surfaces,
the steel door between screens, and the small shield badges. Sources are ours, in art/blitz2026/field_src (the
design agent's round, docs/design: field brief); this writes the game images into art/blitz2026/files, which the app
bundles and lays over the cartridge's own in New Blitz only (Original 1999 keeps the cartridge's art).

  .venv/bin/python tools/make_field_art.py [--rom build/baserom.z64] [--with-optional]

Each image keeps the type, size, flags and origin of the game file it replaces (read from the player's ROM, so
nothing of the game's is copied): midfield halves are type 8 (RGBA5551), stored upside down and split in two
32x64 halves; the doors and two badges are type 0 (256 colours); the rest as the originals.
--with-optional also writes the alternatives: the BLITZ RECOMP door for Midway's, and text-only AFC/NFC badges.
"""
# BRANDING: the midfield, door and badge marks
import argparse
import struct
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "announcer"))
import fs  # noqa: E402
import wms  # noqa: E402

SRC = ROOT / "art" / "blitz2026" / "field_src"
OUT = ROOT / "art" / "blitz2026" / "files"
SURFACES = {"grass": "", "turf": "a", "dark_turf": "b", "dirt": "d", "snow": "s"}
SPRITES = ["doornfl", "vs_nfl", "sp_logo", "nfl122", "nflogo1"]
OPTIONAL = ["doormid", "logoafc", "logonfc"]
# Drawn into the area the original's own picture covers, not its whole canvas: the VS screen shows vs_nfl three times in a
# row, spaced for the shield, which fills only the right 43 columns of its 55 (2026-11-14: ours filled 53, and the three
# ran into each other).
FIT_TO_ORIGINAL = {"vs_nfl"}


def encode(img, ref):
    typ = struct.unpack_from(">I", ref, 4)[0]
    w, h, ox, oy = struct.unpack_from(">IIii", ref, 8)
    if img.size != (w, h):
        raise SystemExit(f"{img.size} isn't the original's {w}x{h}")
    enc = {0: wms.encode_ci8, 8: wms.encode_rgba16, 19: wms.encode_type19}[typ]
    return enc(img, ref[3], ox, oy)


def fit_to(img, box):
    """img's picture, scaled (aspect kept) into box (left, top, right, bottom) of a canvas img's size, against the box's
    right edge and centred down it; hard edges (the game's 1-bit transparency)."""
    pic = img.crop(img.getchannel("A").getbbox())
    bw, bh = box[2] - box[0], box[3] - box[1]
    k = min(bw / pic.width, bh / pic.height)
    pic = pic.resize((max(1, round(pic.width * k)), max(1, round(pic.height * k))), Image.LANCZOS)
    out = Image.new("RGBA", img.size, (0, 0, 0, 0))
    out.alpha_composite(pic, (box[2] - pic.width, box[1] + (bh - pic.height) // 2))
    out.putalpha(out.getchannel("A").point(lambda v: 255 if v >= 128 else 0))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    ap.add_argument("--with-optional", action="store_true")
    a = ap.parse_args()
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    by = fs.by_name(ents)
    OUT.mkdir(parents=True, exist_ok=True)
    written = []
    for surface, suf in SURFACES.items():
        img = Image.open(SRC / f"midfield_{surface}.png").convert("RGBA").transpose(Image.FLIP_TOP_BOTTOM)
        for half, box in (("a", (0, 0, 32, 64)), ("b", (32, 0, 64, 64))):
            name = f"nfl{half}{suf}.wms"
            (OUT / name).write_bytes(encode(img.crop(box), fs.read(rom, by[name])))
            written.append(name)
    for stem in SPRITES + (OPTIONAL if a.with_optional else []):
        name = stem + ".wms"
        ref = fs.read(rom, by[name])
        img = Image.open(SRC / f"{stem}.png").convert("RGBA")
        if stem in FIT_TO_ORIGINAL:
            img = fit_to(img, wms.decode(ref)[0].getchannel("A").getbbox())
        if ref[3] == 2:
            # A texture on the 3D scene (flags 2: nflogo1 is the stadium wall sign), stored upside down like the field's,
            # and opaque on the wall: ours goes on a navy plate inside a gold frame, as the original sign has.
            plate = Image.new("RGBA", img.size, (196, 160, 64, 255))
            inner = Image.new("RGBA", (img.width - 4, img.height - 4), (14, 22, 52, 255))
            plate.paste(inner, (2, 2))
            badge = img.resize((img.width - 6, img.height - 6), Image.LANCZOS)
            plate.alpha_composite(badge, (3, 3))
            img = plate.transpose(Image.FLIP_TOP_BOTTOM)
        (OUT / name).write_bytes(encode(img, ref))
        written.append(name)
    for stem in OPTIONAL if not a.with_optional else []:
        (OUT / f"{stem}.wms").unlink(missing_ok=True)
    print(f"{len(written)} images into {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
