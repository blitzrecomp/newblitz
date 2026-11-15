#!/usr/bin/env python3
"""Show the team's 2027 logos at the game's own sizes, the way the N64 stores them (5 bits per channel, 1-bit alpha,
soft edges darkened), to pick which mark goes in which slot. Writes docs/design/redesign/review/:

  logo_tiers_n64.png       the comparison sheet (shown x3, nearest neighbour)
  badge_source_crop.png    BLITZ over 2027 cut from the app icon, the source for the 64x32 badges

  .venv/bin/python tools/make_logo_tiers.py

Sources: icons/source/logo_newblitz_2027.png, lockup_27.png, app_newblitz_2027.png. Needs Pillow (.venv).
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "icons" / "source"
OUT = ROOT / "docs" / "design" / "redesign" / "review"
BADGE_OUT = ROOT / "icons" / "source" / "badge_source_crop.png"  # tools/make_logo2027.py reads it
FONT = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
BADGE_CROP = (0.06, 0.345, 0.94, 0.865)  # of the icon trimmed to its solid edge: source px (120, 469, 1133, 996)


def trim(path):
    im = Image.open(path).convert("RGBA")
    return im.crop(im.getchannel("A").point(lambda a: 255 if a > 40 else 0).getbbox())


def n64(im, w, h):
    """Fit inside w x h as the game stores it: 5-bit colour, alpha snapped, soft edges darkened."""
    k = min(w / im.width, h / im.height)
    t = im.convert("RGBa").resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS).convert("RGBA")
    px = t.load()
    q = lambda v: (v & 0xF8) | (v >> 5)
    for y in range(t.height):
        for x in range(t.width):
            r, g, b, a = px[x, y]
            if a < 110:
                px[x, y] = (0, 0, 0, 0)
                continue
            if a < 255:
                r, g, b = (int(c * a / 255) for c in (r, g, b))
            px[x, y] = (q(r), q(g), q(b), 255)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.alpha_composite(t, ((w - t.width) // 2, (h - t.height) // 2))
    return out


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    logo, lock, icon = trim(SRC / "logo_newblitz_2027.png"), trim(SRC / "lockup_27.png"), trim(SRC / "app_newblitz_2027.png")
    w, h = icon.size
    badge = icon.crop((int(w * BADGE_CROP[0]), int(h * BADGE_CROP[1]), int(w * BADGE_CROP[2]), int(h * BADGE_CROP[3])))
    badge.save(BADGE_OUT)
    rows = [
        ("Title screen, 384x144 box (now: full logo)", [("full logo", n64(logo, 384, 144), True)]),
        ("Team select, 232x73 (now: full logo)", [("full logo", n64(logo, 232, 73), True), ("lockup", n64(lock, 232, 73), False)]),
        ("New Features corner, 156x64 (now: full logo)", [("full logo", n64(logo, 156, 64), False), ("lockup", n64(lock, 156, 64), True)]),
        ("Badges, 64x32 (now: full logo)", [("full logo", n64(logo, 64, 32), False), ("lockup", n64(lock, 64, 32), False),
                                            ("icon stack", n64(badge, 64, 32), True)]),
    ]
    S = 3
    title, small = ImageFont.truetype(FONT, 22), ImageFont.truetype(FONT, 18)
    height = 20 + sum(40 + max(c.height for _, c, _ in items) * S + 16 + 52 for _, items in rows) + 40
    sheet = Image.new("RGB", (1500, height), (7, 15, 34))
    d = ImageDraw.Draw(sheet)
    y = 20
    for heading, items in rows:
        d.text((24, y), heading, font=title, fill=(246, 212, 0))
        y += 40
        x, tallest = 24, 0
        for name, img, ok in items:
            big = img.resize((img.width * S, img.height * S), Image.NEAREST)
            plate = Image.new("RGBA", (big.width + 16, big.height + 16), (150, 158, 172, 255))
            plate.alpha_composite(big, (8, 8))
            sheet.paste(plate.convert("RGB"), (x, y))
            label = ("USE: " if ok else "no: ") + name
            d.text((x, y + plate.height + 6), label, font=small, fill=(69, 224, 74) if ok else (255, 106, 61))
            x += max(plate.width, int(d.textlength(label, font=small))) + 48
            tallest = max(tallest, plate.height)
        y += tallest + 52
    d.text((24, y), "As the N64 stores them: 5 bits per channel, 1-bit alpha, soft edges darkened. Shown x3, nearest neighbour.",
           font=small, fill=(184, 196, 218))
    sheet.save(OUT / "logo_tiers_n64.png")
    print("wrote", (OUT / "logo_tiers_n64.png").relative_to(ROOT), "and", BADGE_OUT.relative_to(ROOT))


if __name__ == "__main__":
    main()
