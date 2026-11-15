#!/usr/bin/env python3
"""Make the site's web-sized images and fonts from the repo's brand kit (run from the repo root, once, or after the kit
changes). Needs Pillow, fontTools and brotli (pip install pillow fonttools brotli). The outputs are committed, so a
deploy never needs this.

    python3 site/tools/make_assets.py
"""
import os
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "site"
A = SITE / "assets"
SOCIAL = ROOT / "build/brand/social"
UI = ROOT / "docs/design/redesign/ui"


def webp(src, dst, width=None, quality=82, lossless=False):
    im = Image.open(src)
    im = im.convert("RGBA") if im.mode in ("RGBA", "LA", "P") else im.convert("RGB")
    if width and im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    dst.parent.mkdir(parents=True, exist_ok=True)
    im.save(dst, "WEBP", quality=quality, method=6, lossless=lossless)
    print(f"{dst.relative_to(SITE)}  {im.width}x{im.height}  {dst.stat().st_size // 1024} KB")


def png(src, dst, size=None, square=False):
    im = Image.open(src).convert("RGBA")
    if size:
        im = im.resize((size, size) if square else (size, round(im.height * size / im.width)), Image.LANCZOS)
    dst.parent.mkdir(parents=True, exist_ok=True)
    im.save(dst, "PNG", optimize=True)
    print(f"{dst.relative_to(SITE)}  {im.width}x{im.height}  {dst.stat().st_size // 1024} KB")


def jpg(src, dst, width=None, quality=84):
    im = Image.open(src).convert("RGB")
    if width and im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    im.save(dst, "JPEG", quality=quality, optimize=True, progressive=True)
    print(f"{dst.relative_to(SITE)}  {im.width}x{im.height}  {dst.stat().st_size // 1024} KB")


def main():
    img = A / "img"
    # The logo (hero) and the title backdrop.
    logo = ROOT / "icons/source/logo_newblitz_2027.png"
    webp(logo, img / "logo-1600.webp", 1600, quality=86)
    webp(logo, img / "logo-900.webp", 900, quality=86)
    webp(ROOT / "assets/ui/bg_title.png", img / "bg_title-1920.webp", 1920, quality=80)
    webp(ROOT / "assets/ui/bg_title_s.png", img / "bg_title-1000.webp", 1000, quality=78)
    webp(ROOT / "assets/ui/bg_overlay_swoosh_m.png", img / "swoosh-1600.webp", 1600, quality=78)
    webp(ROOT / "docs/media/readme_banner.png", img / "banner-1280.webp", 1280, quality=84)
    webp(ROOT / "icons/app_2026.png", img / "app-icon-256.webp", 256, quality=88)
    # Thumbnails: the trailer slot's poster and the placeholder frame for every screenshot slot.
    webp(SOCIAL / "thumb_trailer.png", img / "thumb_trailer.webp", 1280, quality=80)
    webp(SOCIAL / "thumb_template.png", img / "shot_placeholder.webp", 960, quality=78)
    webp(SOCIAL / "thumb_install.png", img / "thumb_install.webp", 960, quality=80)
    webp(SOCIAL / "thumb_features.png", img / "thumb_features.webp", 960, quality=80)
    # Emoji as feature icons (shown at 48-64 px).
    for name in ["flaming_football", "trophy", "redo", "turbo", "on_fire", "flag", "late_hit", "small_mark"]:
        webp(SOCIAL / f"emoji_{name}.png", img / "emoji" / f"{name}.webp", 128, quality=90)
    # The house kit for CSS (9-slices and corners at 2x).
    for name in ["chrome_corner_tl@2x", "chrome_corner_tr@2x", "chrome_corner_bl@2x", "chrome_corner_br@2x",
                 "header_bevel@2x", "rule_chrome@2x", "selector_cap_l@2x", "selector_cap_r@2x"]:
        png(UI / f"{name}.png", img / "ui" / f"{name}.png")
    panels(img / "ui")
    # Favicons from the small mark.
    mark = SOCIAL / "small_mark_master.png"
    png(mark, SITE / "favicon-32.png", 32, square=True)
    png(mark, SITE / "favicon-192.png", 192, square=True)
    png(mark, SITE / "apple-touch-icon.png", 180, square=True)
    Image.open(mark).convert("RGBA").save(SITE / "favicon.ico", sizes=[(16, 16), (32, 32), (48, 48)])
    # Open Graph / Twitter card.
    jpg(SOCIAL / "og_image.png", img / "og_image.jpg", quality=88)
    # Press kit downloads (full-size files people can take away) and their previews.
    press = A / "press"
    png(logo, press / "newblitz2027-logo.png")
    png(logo, press / "newblitz2027-logo-1200.png", 1200)
    png(SOCIAL / "small_mark_master.png", press / "newblitz-mark-512.png")
    png(SOCIAL / "avatar_master.png", press / "newblitz-avatar-1024.png", 1024, square=True)
    png(ROOT / "icons/app_2026.png", press / "newblitz-app-icon-1024.png")
    png(SOCIAL / "github_social_preview.png", press / "newblitz-banner-1280x640.png")
    png(ROOT / "docs/media/readme_banner.png", press / "newblitz-banner-1280x320.png")
    png(SOCIAL / "og_image.png", press / "newblitz-card-1200x630.png")
    png(SOCIAL / "x_header.png", press / "newblitz-header-1500x500.png")
    for name, src in [("logo", logo), ("mark", mark), ("avatar", SOCIAL / "avatar_master.png"),
                      ("app-icon", ROOT / "icons/app_2026.png"), ("banner", SOCIAL / "github_social_preview.png"),
                      ("strip", ROOT / "docs/media/readme_banner.png"), ("card", SOCIAL / "og_image.png"),
                      ("header", SOCIAL / "x_header.png")]:
        webp(src, press / "preview" / f"{name}.webp", 480, quality=82)
    fonts()


def panels(out):
    """The grid panel as one 9-slice for CSS border-image (slice 32, drawn at 16 px, `round`): the kit's frame corners
    and rims, with a 64 px middle that holds one grid tile, so the grid repeats with the middle."""
    grid = Image.open(UI / "grid_tile@2x.png").convert("RGBA")
    for name in ["panel_frame", "panel_frame_hot"] + [f"panel_frame_p{i}" for i in range(1, 5)]:
        src = Image.open(UI / f"{name}@2x.png").convert("RGBA")  # 96 x 96, slice 32
        dst = Image.new("RGBA", (128, 128))
        c = 32
        dst.paste(src.crop((0, 0, c, c)), (0, 0))
        dst.paste(src.crop((96 - c, 0, 96, c)), (128 - c, 0))
        dst.paste(src.crop((0, 96 - c, c, 96)), (0, 128 - c))
        dst.paste(src.crop((96 - c, 96 - c, 96, 96)), (128 - c, 128 - c))
        for k in (0, 1):
            dst.paste(src.crop((c, 0, 64, c)), (c + k * 32, 0))
            dst.paste(src.crop((c, 96 - c, 64, 96)), (c + k * 32, 128 - c))
            dst.paste(src.crop((0, c, c, 64)), (0, c + k * 32))
            dst.paste(src.crop((96 - c, c, 96, 64)), (128 - c, c + k * 32))
        fill = Image.new("RGBA", (64, 64), src.getpixel((48, 48)))
        fill.alpha_composite(grid)
        dst.paste(fill, (c, c))
        target = out / f"{name.replace('panel_frame', 'panel')}.png"
        dst.save(target, "PNG", optimize=True)
        print(f"{target.relative_to(SITE)}  {target.stat().st_size} B")


def fonts():
    from fontTools import subset
    out = A / "fonts"
    out.mkdir(parents=True, exist_ok=True)
    # Basic Latin, Latin-1, punctuation, arrows and the few symbols the copy uses.
    unicodes = "U+0000-00FF,U+0131,U+0152-0153,U+02C6,U+02DA,U+02DC,U+2000-206F,U+2190-2193,U+2122,U+2190-21FF,U+2212,U+2715,U+25A1,U+25B3,U+25CB,U+00D7"
    for src, dst in [("BarlowCondensed-ExtraBoldItalic.ttf", "barlow-condensed-800i.woff2"),
                     ("BarlowCondensed-BoldItalic.ttf", "barlow-condensed-700i.woff2"),
                     ("InterVariable.ttf", "inter-var.woff2")]:
        args = [str(ROOT / "assets" / src), f"--unicodes={unicodes}", "--flavor=woff2", "--layout-features=kern,liga,calt,tnum,case",
                f"--output-file={out / dst}", "--no-hinting", "--desubroutinize"]
        subset.main(args)
        print(f"fonts/{dst}  {(out / dst).stat().st_size // 1024} KB")
    for lic in ["BarlowCondensed-OFL.txt", "Inter-OFL.txt"]:
        (out / lic).write_bytes((ROOT / "assets" / lic).read_bytes())


if __name__ == "__main__":
    os.chdir(ROOT)
    sys.exit(main())
