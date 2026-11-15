#!/usr/bin/env python3
"""The PS5 home screen's pictures for a title (docs/PS5_HOMEBREW.md), from the app's own launcher logo:
pic0.png (the background behind the title's tile: the logo to the right, clear of the tiles) and pic1.png (the
splash while it starts: the logo centred), both 3840x2160 on the Blitz '27 night-navy ground. png_to_bc7_dds turns
them into the console's DDS files.
    make_title_art.py <logo.png> <out dir>
"""
# BRANDING: PS5 title art (Blitz '27 logo)
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

W, H = 3840, 2160
GROUND = (2, 7, 18)
NAVY = (13, 26, 58)
GLOW = (63, 10, 95)


def ground() -> Image.Image:
    """Night navy with a purple glow at the top right and a navy one at the bottom left (the Roster Room's page)."""
    img = Image.new("RGB", (W, H), GROUND)
    glow = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(glow)
    d.ellipse((int(W * 0.45), int(-H * 0.6), int(W * 1.55), int(H * 0.6)), fill=GLOW)
    d.ellipse((int(-W * 0.5), int(H * 0.45), int(W * 0.5), int(H * 1.6)), fill=NAVY)
    glow = glow.filter(ImageFilter.GaussianBlur(420))
    return Image.blend(img, glow, 0.85)


def place(base: Image.Image, logo: Image.Image, width: int, cx: float, cy: float) -> Image.Image:
    scale = width / logo.width
    sized = logo.resize((width, int(logo.height * scale)), Image.LANCZOS)
    out = base.copy()
    out.paste(sized, (int(W * cx - sized.width / 2), int(H * cy - sized.height / 2)), sized)
    return out


def main() -> None:
    logo_path, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    logo = Image.open(logo_path).convert("RGBA")
    g = ground()
    place(g, logo, 1500, 0.70, 0.36).save(out / "pic0.png")
    place(g, logo, 1900, 0.50, 0.50).save(out / "pic1.png")


if __name__ == "__main__":
    main()
