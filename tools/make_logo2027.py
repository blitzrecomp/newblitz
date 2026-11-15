#!/usr/bin/env python3
"""the team's NFL Blitz 2027 logo (icons/source/logo_newblitz_2027.png) as game images for Modern Blitz, bundled with
the app (art/blitz2026/files/, copied into Resources/art/files; src/game/art_dropin.cpp loads them):

  blitzlog.wms   232x73, the team select and choose-opponent logo (origin 108 200, flags 4)

  .venv/bin/python tools/make_logo2027.py

With --overlays it writes art/blitz2026/logo27/*.ovl: the team's marks fitted to each slot, bundled with Modern Blitz,
which lays them over the player's own title, New Features and badge images at startup (src/game/art_dropin.cpp).
That is how every copy gets the 2027 logo; nothing from the ROM is shipped.

With --from-rom it does the same thing offline, to preview it or to hand-edit the result, written to
<app folder>/art/files/ (default build/appdata2026; a file there wins over the startup version), never to git:

  blitztit.wms   the title screen (512x256): the logo over the 2000 one, bottom centre
  nf_bg.wms      New Features (512x256): the BLITZ '27 lockup, top right
  blitzmm.wms, blitz_logo.wms, blitzlogo.wms, blitz2000.wms   the 64x32 badges: BLITZ over 2027 from the app icon

  .venv/bin/python tools/make_logo2027.py --from-rom [--rom build/baserom.z64] [--app-folder DIR] [--preview DIR]

The N64 images have 1-bit alpha: edges are snapped and soft edge pixels darkened, so there are no halos.
"""
# BRANDING: draws the NFL Blitz 2027 / BLITZ '27 logos
import argparse
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "icons" / "source" / "logo_newblitz_2027.png"
LOCKUP = ROOT / "icons" / "source" / "lockup_27.png"   # BLITZ '27, for the New Features corner
BADGE = ROOT / "icons" / "source" / "badge_source_crop.png"  # BLITZ over 2027 (tools/make_logo_tiers.py)
OUT = ROOT / "art" / "blitz2026" / "files"
BUILD = ROOT / "build" / "art"


def trimmed(path):
    im = Image.open(path).convert("RGBA")
    return im.crop(im.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox())


def fit(im, w, h):
    """Scale to fit w x h, centred on a transparent canvas."""
    k = min(w / im.width, h / im.height)
    small = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    canvas.paste(small, ((w - small.width) // 2, (h - small.height) // 2), small)
    return canvas


def one_bit(im):
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a < 110:
                px[x, y] = (0, 0, 0, 0)
            elif a < 255:
                k = a / 255
                px[x, y] = (int(r * k), int(g * k), int(b * k), 255)
    return im


def encode(png, wms, origin, flags):
    subprocess.run([sys.executable, str(ROOT / "tools" / "wms.py"), "encode", str(png), str(wms),
                    "--origin", str(origin[0]), str(origin[1]), "--flags", str(flags)], check=True)


def from_rom(rom_path, app_folder, preview):
    sys.path.insert(0, str(ROOT / "tools"))
    sys.path.insert(0, str(ROOT / "tools" / "announcer"))
    import fs
    import wms
    rom = fs.load(rom_path)
    _, ents = fs.toc(rom)
    by = fs.by_name(ents)
    logo = trimmed(SRC)
    # The full logo doesn't read at 64x32 (review S11). Upside down: these images are stored so, and the stadium's signs
    # turn them over (the ROM's own read upside down in the file too).
    badge = ImageOps.flip(Image.open(BADGE).convert("RGBA"))
    out_dir = Path(app_folder) / "art" / "files"
    out_dir.mkdir(parents=True, exist_ok=True)

    def load(name):
        img, info = wms.decode(fs.read(rom, by[name]))
        return img.convert("RGBA"), info

    def save(name, img, info):
        if info["type"] != 8:
            raise SystemExit(f"{name}: type {info['type']} isn't written here")
        (out_dir / name).write_bytes(wms.encode_rgba16(img, info["flags"], *info["origin"]))
        if preview:
            Path(preview).mkdir(parents=True, exist_ok=True)
            img.save(Path(preview) / name.replace(".wms", ".png"))
        print(f"{name}: NFL Blitz 2027 logo")

    def paste(img, box, art_src=None):
        x0, y0, x1, y1 = box
        art = fit(logo if art_src is None else art_src, x1 - x0, y1 - y0)
        img.paste(art, (x0, y0), art)

    # Title: the 2000 logo spans about x 140-505, y 128-250; the 2027 logo (with its flame) covers it.
    img, info = load("blitztit.wms")
    paste(img, (128, 112, 512, 256))
    save("blitztit.wms", img, info)
    # New Features: top right, clear of "NEW FEATURES" (ends at x ~350).
    img, info = load("nf_bg.wms")
    # First erase the old logo: that corner is plain chrome below the logo, so each column above y 84 is
    # rebuilt from the pixel just under the logo, fading in from x 336 to 368 so there's no seam; any
    # saturated (logo-coloured) pixel left of that goes too.
    # Rows 0-28 (above the text, whose top is at y ~30) start further left, at x 300 where that corner is plain chrome, fading over
    # x 300-324, so no seam is left at x ~316-330 (review S11); rows 29-84 keep the old start, clear of the S.
    px = img.load()
    orig_px = img.copy().load()
    for x in range(300, 512):
        base = orig_px[x, 84]
        for y in range(0, 84):
            if y < 29:
                w = min(1.0, max(0.0, (x - 300) / 24))
            elif x < 320:
                continue
            else:
                w = min(1.0, max(0.0, (x - 336) / 32))
            orig = orig_px[x, y]
            sat = max(orig[:3]) - min(orig[:3]) > 40
            ww = 1.0 if (sat and x >= 320) else w
            px[x, y] = tuple(int(orig[c] * (1 - ww) + base[c] * ww) for c in range(3)) + (255,)
    paste(img, (356, 2, 512, 66), trimmed(LOCKUP))
    save("nf_bg.wms", img, info)
    # Small badges: clear the inside to the badge's own ground, then the logo.
    for name in ("blitzmm.wms", "blitz_logo.wms", "blitzlogo.wms", "blitz2000.wms"):
        img, info = load(name)
        ground = img.getpixel((img.width // 2, 1)) if name != "blitz_logo.wms" else (0, 0, 0, 0)
        inset = 3 if name == "blitzmm.wms" else 0
        for y in range(inset, img.height - inset):
            for x in range(inset, img.width - inset):
                img.putpixel((x, y), ground)
        paste(img, (inset + 1, inset + 1, img.width - inset - 1, img.height - inset - 1), badge)
        save(name, one_bit(img) if name == "blitz_logo.wms" else img, info)
    print(f"wrote 6 images to {out_dir}; they load at the app's next start")


OVERLAYS = ROOT / "art" / "blitz2026" / "logo27"


def write_overlays():
    """the team's marks, fitted to each slot, as raw overlays the app lays over the player's own ROM images at
    startup (src/game/art_dropin.cpp), so every Modern Blitz gets the 2027 logos without shipping game art.
    Format: "LOV1", big-endian u16 x, y, width, height, then RGBA bytes."""
    import struct
    OVERLAYS.mkdir(parents=True, exist_ok=True)
    logo, lockup = trimmed(SRC), trimmed(LOCKUP)
    badge = ImageOps.flip(Image.open(BADGE).convert("RGBA"))  # stored upside down, as in from_rom

    def put(name, art, box):
        x0, y0, x1, y1 = box
        img = fit(art, x1 - x0, y1 - y0)
        (OVERLAYS / f"{name}.ovl").write_bytes(b"LOV1" + struct.pack(">HHHH", x0, y0, img.width, img.height) + img.tobytes())

    put("blitztit", logo, (128, 112, 512, 256))   # the title screen (512x256)
    put("nf_bg", lockup, (356, 2, 512, 66))       # New Features, top right
    put("blitzmm", badge, (4, 4, 60, 28))         # 64x32 badges (blitzmm keeps a 3 px frame)
    for name in ("blitz_logo", "blitzlogo", "blitz2000"):
        put(name, badge, (1, 1, 63, 31))
    print("wrote", len(list(OVERLAYS.glob("*.ovl"))), "overlays to", OVERLAYS)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from-rom", action="store_true")
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    ap.add_argument("--app-folder", default=str(ROOT / "build" / "appdata2026"))
    ap.add_argument("--preview")
    ap.add_argument("--overlays", action="store_true", help="write the bundled overlays (art/blitz2026/logo27)")
    a = ap.parse_args()
    if a.overlays:
        write_overlays()
        return
    if a.from_rom:
        from_rom(a.rom, a.app_folder, a.preview)
        return
    OUT.mkdir(parents=True, exist_ok=True)
    BUILD.mkdir(parents=True, exist_ok=True)
    logo = trimmed(SRC)
    team_select = one_bit(fit(logo, 232, 73))
    team_select.save(BUILD / "blitzlog2027_232x73.png")
    encode(BUILD / "blitzlog2027_232x73.png", OUT / "blitzlog.wms", (108, 200), 4)
    print("wrote", OUT / "blitzlog.wms")


if __name__ == "__main__":
    main()
