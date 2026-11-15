#!/usr/bin/env python3
"""App icons (our own art): "BLITZ" over "'27" (2026 app) or "2000" (Classic), in the logo's chrome
blue and gold (tools/make_logo27.py), on a rounded navy tile with a gold edge, 1024x1024.

Writes icons/app_classic.png (and app_2026.png, but the Modern Blitz icon is now the team's art from icons/source/app_newblitz_2027.png: run with care). Needs Pillow (.venv) and macOS's Impact font.
"""
# BRANDING: app icons and logos
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from make_logo27 import IMPACT, gradient, paint, text_mask  # noqa: E402

SIZE = 1024


def centred(text, font, y, size):
    w = ImageDraw.Draw(Image.new("L", (1, 1))).textlength(text, font=font)
    return text_mask(text, font, size, ((size[0] - w) / 2, y), shear=-0.18)


def make(year, out_path):
    tile = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    margin, radius = 100, 185
    box = (margin, margin, SIZE - margin, SIZE - margin)
    shadow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((box[0], box[1] + 14, box[2], box[3] + 14), radius, fill=(0, 0, 0, 120))
    tile.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(18)))
    grad = gradient((SIZE, SIZE), [(0.0, (26, 30, 84, 255)), (1.0, (8, 10, 30, 255))])
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius, fill=255)
    tile.paste(grad, (0, 0), mask)

    # Red chevron behind the type, as in the logo.
    art = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(art).polygon([(190, 470), (834, 470), (512, 640)], fill=(196, 24, 36, 255))
    tile.alpha_composite(art)

    f_bl = ImageFont.truetype(IMPACT, 300)
    m_bl = centred("BLITZ", f_bl, 175, (SIZE, SIZE))
    paint(tile, m_bl.filter(ImageFilter.MaxFilter(17)), (8, 14, 50, 255))
    paint(tile, m_bl.filter(ImageFilter.MaxFilter(9)), (240, 245, 255, 255))
    paint(tile, m_bl, gradient((SIZE, SIZE), [(0.0, (120, 190, 255, 255)), (0.33, (30, 70, 200, 255)),
                                              (0.40, (200, 225, 255, 255)), (0.50, (20, 50, 170, 255)),
                                              (1.0, (20, 50, 170, 255))]))
    f_yr = ImageFont.truetype(IMPACT, 330 if year == "'27" else 250)
    m_yr = centred(year, f_yr, 470 if year == "'27" else 520, (SIZE, SIZE))
    paint(tile, m_yr.filter(ImageFilter.MaxFilter(17)), (60, 20, 0, 255))
    paint(tile, m_yr.filter(ImageFilter.MaxFilter(9)), (255, 250, 210, 255))
    paint(tile, m_yr, gradient((SIZE, SIZE), [(0.0, (255, 240, 120, 255)), (0.45, (255, 240, 120, 255)),
                                       (0.65, (255, 170, 20, 255)), (0.85, (220, 90, 0, 255)), (1.0, (220, 90, 0, 255))]))

    # Keep everything inside the tile, then the gold edge on top.
    clipped = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    clipped.paste(tile, (0, 0), mask)
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(18)))
    out.alpha_composite(clipped)
    ImageDraw.Draw(out).rounded_rectangle(box, radius, outline=(255, 204, 26, 255), width=14)
    out.save(out_path)
    print("wrote", out_path)


if __name__ == "__main__":
    make("'27", ROOT / "icons" / "app_2026.png")
    make("2000", ROOT / "icons" / "app_classic.png")
