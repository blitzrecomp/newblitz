#!/usr/bin/env python3
"""Draw the "NFL Blitz '27" logo (our own artwork) for the 2026 build's team-select screen.

Writes build/art/blitzlog27.png (preview, 4x) and build/art/blitzlog27_232x73.png (game size).
`--year 2000` draws the Classic launcher's variant instead (build/art/blitzlog2000.png, preview only).
tools/wms.py then converts the game-size PNG to the game's .wms format; see packs/README.md.
Needs Pillow (.venv). Fonts: macOS Impact / Arial Black.
"""
# BRANDING: draws the NFL Blitz '27 logo
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "art"
S = 4                      # supersampling
W, H = 232 * S, 73 * S
IMPACT = "/System/Library/Fonts/Supplemental/Impact.ttf"
BLACK = "/System/Library/Fonts/Supplemental/Arial Black.ttf"


def gradient(size, stops, horizontal=False):
    w, h = size
    g = Image.new("RGBA", size)
    px = g.load()
    n = w if horizontal else h
    for i in range(n):
        t = i / max(1, n - 1)
        for k in range(len(stops) - 1):
            t0, c0 = stops[k]
            t1, c1 = stops[k + 1]
            if t0 <= t <= t1:
                u = (t - t0) / (t1 - t0) if t1 > t0 else 0
                c = tuple(int(c0[j] + (c1[j] - c0[j]) * u) for j in range(4))
                break
        if horizontal:
            for y in range(h):
                px[i, y] = c
        else:
            for x in range(w):
                px[x, i] = c
    return g


def text_mask(text, font, size, pos, shear=0.0, stroke=0):
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    d.text(pos, text, font=font, fill=255, stroke_width=stroke, stroke_fill=255)
    if shear:
        m = m.transform(size, Image.AFFINE, (1, shear, -shear * size[1] / 2, 0, 1, 0), resample=Image.BICUBIC)
    return m


def paint(base, mask, fill):
    layer = fill if isinstance(fill, Image.Image) else Image.new("RGBA", base.size, fill)
    base.paste(layer, (0, 0), mask)


def main():
    import sys
    year = sys.argv[sys.argv.index("--year") + 1] if "--year" in sys.argv else "'27"
    OUT.mkdir(parents=True, exist_ok=True)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Flame streak behind everything, sweeping from the right edge into the '27.
    flame = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    fd = ImageDraw.Draw(flame)
    for i in range(60):
        t = i / 59
        cx = int(W * (0.52 + 0.44 * t))
        cy = int(H * (0.78 - 0.55 * t * t))
        r = int(S * (16 - 11 * t))
        col = (255, int(200 - 140 * t), int(40 * (1 - t)), int(235 * (1 - 0.6 * t)))
        fd.ellipse((cx - r, cy - r, cx + r, cy + r), fill=col)
    flame = flame.filter(ImageFilter.GaussianBlur(S * 2))
    img.alpha_composite(flame)

    # Red/blue chevron band (an original take on the classic Blitz banner).
    band_y = int(H * 0.30)
    band = [(int(W * 0.04), band_y), (int(W * 0.70), band_y), (int(W * 0.64), band_y + 13 * S),
            (int(W * 0.38), int(H * 0.88)), (int(W * 0.10), band_y + 13 * S)]
    d.polygon(band, fill=(18, 32, 110, 255))
    inner = [(x + (6 * S if i in (0, 4) else -6 * S if i in (1, 2) else 0), y + 3 * S) for i, (x, y) in enumerate(band)]
    d.polygon(inner, fill=(196, 24, 36, 255))

    # "NFL" in chrome across the top.
    f_nfl = ImageFont.truetype(BLACK, 15 * S)
    m_nfl = text_mask("NFL", f_nfl, (W, H), (int(W * 0.24), int(H * -0.04)), shear=-0.18)
    paint(img, m_nfl.filter(ImageFilter.MaxFilter(2 * S + 1)), (10, 18, 60, 255))
    paint(img, m_nfl, gradient((W, H), [(0.0, (255, 255, 255, 255)), (0.18, (150, 170, 210, 255)), (0.3, (235, 240, 255, 255))]))

    # "BLITZ": big italic, blue-to-white chrome with a white rim and a dark outline.
    f_bl = ImageFont.truetype(IMPACT, 44 * S)
    pos = (int(W * 0.06), int(H * 0.12))
    m_bl = text_mask("BLITZ", f_bl, (W, H), pos, shear=-0.22)
    shadow = m_bl.filter(ImageFilter.MaxFilter(5 * S + 1)).transform((W, H), Image.AFFINE, (1, 0, -2 * S, 0, 1, -2 * S))
    paint(img, shadow, (0, 0, 0, 160))
    paint(img, m_bl.filter(ImageFilter.MaxFilter(4 * S + 1)), (8, 14, 50, 255))
    paint(img, m_bl.filter(ImageFilter.MaxFilter(2 * S + 1)), (240, 245, 255, 255))
    paint(img, m_bl, gradient((W, H), [(0.0, (120, 190, 255, 255)), (0.45, (30, 70, 200, 255)),
                                        (0.55, (200, 225, 255, 255)), (0.75, (20, 50, 170, 255))]))

    # Football riding the flame.
    fx, fy, fr = int(W * 0.50), int(H * 0.80), 9 * S
    ball = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bd = ImageDraw.Draw(ball)
    bd.ellipse((fx - int(fr * 1.6), fy - fr, fx + int(fr * 1.6), fy + fr), fill=(120, 62, 28, 255), outline=(30, 14, 6, 255), width=S)
    bd.line((fx - fr, fy, fx + fr, fy), fill=(250, 245, 235, 255), width=S)
    for k in range(-3, 4):
        bd.line((fx + k * S * 2, fy - S * 2, fx + k * S * 2, fy + S * 2), fill=(250, 245, 235, 255), width=max(1, S // 2))
    ball = ball.rotate(-20, center=(fx, fy), resample=Image.BICUBIC)
    img.alpha_composite(ball)

    # "'27" in gold, overlapping the band's tip.
    f_27 = ImageFont.truetype(IMPACT, (46 if year == "'27" else 30) * S)
    pos_27 = (int(W * 0.60), int(H * 0.18)) if year == "'27" else (int(W * 0.58), int(H * 0.40))
    m_27 = text_mask(year, f_27, (W, H), pos_27, shear=-0.22)
    paint(img, m_27.filter(ImageFilter.MaxFilter(4 * S + 1)), (60, 20, 0, 255))
    paint(img, m_27.filter(ImageFilter.MaxFilter(2 * S + 1)), (255, 250, 210, 255))
    paint(img, m_27, gradient((W, H), [(0.0, (255, 240, 120, 255)), (0.5, (255, 170, 20, 255)), (1.0, (220, 90, 0, 255))]))

    if year != "'27":
        img.save(OUT / f"blitzlog{year}.png")
        print("wrote", OUT / f"blitzlog{year}.png")
        return
    img.save(OUT / "blitzlog27.png")
    small = img.resize((232, 73), Image.LANCZOS)
    # The N64 format has 1-bit alpha: snap it, and darken soft edge pixels so there are no halos.
    px = small.load()
    for y in range(73):
        for x in range(232):
            r, g, b, a = px[x, y]
            if a < 110:
                px[x, y] = (0, 0, 0, 0)
            elif a < 255:
                k = a / 255
                px[x, y] = (int(r * k), int(g * k), int(b * k), 255)
    small.save(OUT / "blitzlog27_232x73.png")
    print("wrote", OUT / "blitzlog27.png", "and", OUT / "blitzlog27_232x73.png")


if __name__ == "__main__":
    main()
