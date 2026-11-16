#!/usr/bin/env python3
"""The Mac disk image's window background: navy, the NEW BLITZ 2027 logo at the top, a yellow arrow from the app's
spot to the Applications folder's, and "Drag New Blitz to Applications" under them.

  .venv/bin/python tools/make_dmg_background.py <out folder>
      writes <out>/background.png (660x440) and <out>/background@2x.png (1320x880)

tools/release/make_downloads.sh makes it at build time; platform/macos/dmg_settings.py puts the icons on its spots
(the numbers below and there must agree). Needs Pillow (.venv). Fonts: assets/ (Inter); logo: icons/logo_2026.png
(the launcher's logo: no NFL wordmark).
"""
# BRANDING: the Mac disk image background
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
LOGO = ROOT / "icons" / "logo_2026.png"
INTER = ROOT / "assets" / "InterVariable.ttf"

# Points. dmg_settings.py: icons 128 (text 13), app at (170, 192), Applications at (490, 192). The window shows 660x400
# of it under the title bar; the image runs 40 points further down so a shorter title bar never shows white below it.
W, H = 660, 440
APP_X, APPS_X, ICON_Y, ICON = 170, 490, 192, 128
LABEL_Y = ICON_Y + 83      # the centre line of the Finder's own name label under each icon
CAPTION_Y = 324
LOGO_W, LOGO_TOP = 236, 22

NAVY_TOP = (11, 22, 56)    # #0B1638
NAVY_BOTTOM = (6, 12, 32)  # #060C20
GLOW = (32, 56, 140)
YELLOW = (255, 214, 0)     # #FFD600
ORANGE = (255, 150, 0)
# The name plates behind the Finder's labels: the Finder draws them black in Light Mode and white in Dark Mode, and
# this blue reads about 4.5:1 against either.
PLATE = (74, 108, 240)     # #4A6CF0
CAPTION = (176, 192, 232)


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def render(scale):
    w, h = W * scale, H * scale
    s = lambda v: round(v * scale)

    # Navy, lighter at the top.
    bg = Image.new("RGB", (1, h))
    for y in range(h):
        bg.putpixel((0, y), lerp(NAVY_TOP, NAVY_BOTTOM, min(1, y / s(400)) ** 0.8))
    bg = bg.resize((w, h)).convert("RGBA")

    # A soft glow behind the logo and each icon spot.
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    for cx, cy, r, a in ((W / 2, 64, 190, 70), (APP_X, ICON_Y, 105, 60), (APPS_X, ICON_Y, 105, 60)):
        g.ellipse((s(cx - r), s(cy - r * 0.62), s(cx + r), s(cy + r * 0.62)), fill=GLOW + (a,))
    glow = glow.filter(ImageFilter.GaussianBlur(s(38)))
    bg.alpha_composite(glow)

    # The arrow: italic like the logo, yellow to orange, with three speed lines behind it. Drawn at 4x and shrunk.
    k = 4
    ar = Image.new("RGBA", (w * k, h * k), (0, 0, 0, 0))
    a = ImageDraw.Draw(ar)
    S = lambda v: round(v * scale * k)
    x0, x1, cy = APP_X + ICON / 2 + 34, APPS_X - ICON / 2 - 30, ICON_Y
    shaft, head_h, head_w = 7, 21, 30
    slant = 0.22                                     # x shift per point above the centre line
    sk = lambda x, y: (S(x + (cy - y) * slant), S(y))
    arrow = [sk(x0, cy - shaft), sk(x1 - head_w, cy - shaft), sk(x1 - head_w, cy - head_h), sk(x1, cy),
             sk(x1 - head_w, cy + head_h), sk(x1 - head_w, cy + shaft), sk(x0, cy + shaft)]
    a.polygon(arrow, fill=(255, 255, 255, 255))
    xe = x0 - 8
    for dy, ln in ((-12, 18), (0, 28), (12, 18)):
        a.polygon([sk(xe - ln, cy + dy - 2), sk(xe, cy + dy - 2), sk(xe, cy + dy + 2), sk(xe - ln, cy + dy + 2)],
                  fill=(255, 255, 255, 255))
    mask = ar.getchannel("A").resize((w, h), Image.LANCZOS)
    grad = Image.new("RGB", (w, 1))
    left, right = s(x0 - 40), s(x1)
    for x in range(w):
        t = min(1, max(0, (x - left) / max(1, right - left)))
        grad.putpixel((x, 0), lerp(YELLOW, ORANGE, t ** 1.6))
    grad = grad.resize((w, h)).convert("RGBA")
    grad.putalpha(mask)
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    shadow.putalpha(mask.point(lambda v: v * 0.55))
    shadow = shadow.filter(ImageFilter.GaussianBlur(s(3)))
    bg.alpha_composite(shadow, (0, s(2)))
    bg.alpha_composite(grad)

    # A slanted blue plate under each icon, where the Finder writes its name.
    pl = Image.new("RGBA", (w * k, h * k), (0, 0, 0, 0))
    p = ImageDraw.Draw(pl)
    pw, ph = 112, 22
    for cx in (APP_X, APPS_X):
        t, b = LABEL_Y - ph / 2, LABEL_Y + ph / 2
        p.polygon([(S(cx - pw / 2 + 5), S(t)), (S(cx + pw / 2 + 5), S(t)), (S(cx + pw / 2 - 5), S(b)),
                   (S(cx - pw / 2 - 5), S(b))], fill=PLATE + (255,))
    bg.alpha_composite(pl.resize((w, h), Image.LANCZOS))

    # The logo, top centre.
    logo = Image.open(LOGO).convert("RGBA")
    lw = s(LOGO_W)
    logo = logo.resize((lw, round(lw * logo.height / logo.width)), Image.LANCZOS)
    bg.alpha_composite(logo, ((w - lw) // 2, s(LOGO_TOP)))

    # The caption.
    font = ImageFont.truetype(str(INTER), s(13))
    try:
        font.set_variation_by_axes([550])
    except Exception:
        pass
    d = ImageDraw.Draw(bg)
    text = "Drag New Blitz to Applications"
    d.text((w / 2, s(CAPTION_Y)), text, font=font, fill=CAPTION + (255,), anchor="mm")
    return bg.convert("RGB")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for scale, name in ((1, "background.png"), (2, "background@2x.png")):
        im = render(scale)
        # 72 and 144 dpi, so the Finder (and tiffutil -cathidpicheck) treat the 2x file as the same size in points.
        im.save(out / name, dpi=(72 * scale, 72 * scale), optimize=True)
    print(f"{out}/background.png, background@2x.png")


if __name__ == "__main__":
    main()
