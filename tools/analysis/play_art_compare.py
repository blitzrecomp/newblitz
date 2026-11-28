#!/usr/bin/env python3
"""Contact sheet: each play-select tile (false colour) beside its decoded routes, raw and with the
left-of-ball receivers mirrored. Analysis only (docs/notes/PLAY_ART.md).

  play_art_compare.py OUT.png [--rom build/baserom.z64]
"""
import argparse
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from play_routes import ROOT, UNITS_PER_YARD, Rom, load_plays, trace  # noqa: E402

FS = 0x150C40  # asset file system ("fs" magic) in the vanilla ROM

# Index -> false colour (the tiles' own palettes are empty; the game supplies colours at draw time).
PAL = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (255, 0, 255), (0, 255, 255),
       (255, 128, 0), (128, 0, 255), (0, 128, 64), (128, 128, 128), (255, 255, 255), (128, 64, 0),
       (64, 64, 160), (200, 160, 160), (160, 200, 160)]
SLOT_COLOURS = {3: (255, 255, 0), 4: (255, 80, 80), 5: (80, 255, 80), 6: (80, 160, 255)}
ROWS = [("QB Post", "qbpost.wms"), ("Switch Up", "swtchup.wms"), ("Zig Zag", "zigzag.wms"),
        ("Cruisin'", "cruisin.wms"), ("Upper Cut", "uprct.wms"), ("Slip Slide", "slpsld.wms"),
        ("Criss X", "criscros.wms"), ("Hail Mary", "hlmry.wms")]


def fs_files(rom: bytes):
    tocoff, n = struct.unpack_from(">II", rom, FS + 8)
    out = {}
    for i in range(n):
        e = FS + tocoff + i * 40
        name = rom[e:e + 0x18].split(b"\0")[0].decode().lower()
        usize, off, typ, psize = struct.unpack_from(">IIII", rom, e + 0x18)
        out[name] = (FS + off, usize, typ, psize)
    return out


def tile(rom: bytes, files, name, scale=4):
    off, usize, typ, psize = files[name]
    if typ == 5:
        d = rom[off:off + usize]
    else:
        import lzo  # tools/analysis/lzo.py (pure-Python LZO1X)
        d = lzo.decompress(rom[off:off + psize], usize)
    t, w, h = struct.unpack_from(">III", d, 4)
    p = d[32:]
    stride = (w + 1) & ~1          # ci4 rows are padded to an even pixel count (77 -> 78)
    idx = [(p[(y * stride + x) // 2] >> (4 if (y * stride + x) % 2 == 0 else 0)) & 15
           for y in range(h) for x in range(stride)]
    im = Image.new("RGB", (stride, h))
    im.putdata([PAL[v] for v in idx])
    return im.resize((stride * scale, h * scale), Image.NEAREST)


def diagram(play, mirror_left, size=(312, 244)):
    im = Image.new("RGB", size, (20, 60, 20))
    d = ImageDraw.Draw(im)
    s, ox, oy = 6, size[0] // 2, 180

    def P(D, L):
        return (ox + L * s, oy - D * s)

    d.line([P(0, -26), P(0, 26)], fill=(200, 200, 200))
    for pl in play["players"]:
        x0, z0 = pl["x"] / UNITS_PER_YARD, pl["z"] / UNITS_PER_YARD
        cx, cy = P(x0, z0)
        d.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], outline=(255, 255, 255))
        if pl["slot"] < 3:
            continue
        pts, _ = trace((x0, z0), [tuple(o) for o in pl["ops"]], flipped=(mirror_left and z0 < 0))
        if len(pts) > 1:
            d.line([P(*q) for q in pts], fill=SLOT_COLOURS[pl["slot"]], width=2)
    d.text((4, 4), play["name"] + ("  left side mirrored" if mirror_left else "  raw"), fill=(255, 255, 255))
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    a = ap.parse_args()
    raw = Path(a.rom).read_bytes()
    files = fs_files(raw)
    plays = {p["name"]: p for p in load_plays(Rom(a.rom))}
    w, h = 312, 244
    sheet = Image.new("RGB", (w * 3, h * len(ROWS)))
    for i, (name, tname) in enumerate(ROWS):
        sheet.paste(tile(raw, files, tname), (0, h * i))
        sheet.paste(diagram(plays[name], False), (w, h * i))
        sheet.paste(diagram(plays[name], True), (2 * w, h * i))
    sheet.save(a.out)


if __name__ == "__main__":
    main()
