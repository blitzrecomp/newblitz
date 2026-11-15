#!/usr/bin/env python3
"""Button icons for each controller family, in place of the game's N64 ones (our own art): glossy
buttons with a soft drop shadow, at the sizes and origins of the originals (n64bttn*.wms).

Writes assets/pad_glyphs/<family>_<glyph>.wms (type 19: colour + 4-bit alpha, tools/wms.py), previews
in build/art/pad_glyphs/ and a review sheet at build/analysis/pad_glyphs_sheet.png. The app adds them
to the game's file system as ~<family>_<glyph>.wms and shows each player their own controller's icons
(src/game/button_glyphs.cpp). Families: ps (PlayStation), xb (Xbox), sw (Switch, by position: the
south button is labelled B), rs (the right stick, shared by all three; the N64's C buttons).

Mapping (our default controller bindings): N64 A = south, B = west, Z = left trigger, L = left
shoulder, R = right trigger, C buttons = right stick.
  ps: A = Cross, B = Square, Z = L2, L = L1, R = R2
  xb: A = A,     B = X,      Z = LT, L = LB, R = RT
  sw: A = B,     B = Y,      Z = ZL, L = L,  R = ZR
Only button letters and shapes: no logos. Needs Pillow (.venv) and macOS's Arial Black.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import wms  # noqa: E402

S = 8  # supersampling
# The game draws these sprites narrower than their pixels (measured: a 28x18 oval in the originals
# shows as a circle), so each icon is drawn in on-screen proportions, AX times its width, and stretched
# to its size at the end.
AX = 0.68
BOLD = "/System/Library/Fonts/Supplemental/Arial Black.ttf"
OUT = ROOT / "assets" / "pad_glyphs"
PREVIEW = ROOT / "build" / "art" / "pad_glyphs"
SHEET = ROOT / "build" / "analysis" / "pad_glyphs_sheet.png"
WHITE = (245, 246, 250, 255)
PS_BLUE, PS_PINK = (110, 170, 255, 255), (255, 140, 205, 255)
XB_GREEN, XB_BLUE = (96, 210, 80, 255), (70, 150, 255, 255)


def wide(w):
    """The on-screen width of a w-pixel-wide icon."""
    return round(w * AX)


def canvas(w, h):
    return Image.new("RGBA", (wide(w) * S, h * S), (0, 0, 0, 0))


def finish(img, w, h):
    """Drop shadow (down-right, soft), downscale, and keep alpha >= 1/16 visible."""
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    a = img.split()[3].point(lambda v: int(v * 0.65))
    shadow.paste((0, 0, 0, 255), (int(1.5 * AX * S), int(1.5 * S)), a)
    shadow = shadow.filter(ImageFilter.GaussianBlur(S * 0.8))
    out = Image.alpha_composite(shadow, img)
    return out.resize((w, h), Image.LANCZOS)


def glossy_oval(d, box):
    """A dark glossy button body (an oval like the originals)."""
    x0, y0, x1, y1 = box
    d.ellipse(box, fill=(18, 20, 28, 255), outline=(150, 156, 170, 255), width=S)
    hl = (x0 + S * 3, y0 + S * 2, x1 - S * 3, (y0 + y1) // 2)  # highlight on the upper half
    d.ellipse(hl, fill=(70, 74, 92, 255))


def face_button(symbol, colour):
    """A face button (35x22): a PlayStation symbol (cross / square) or a letter."""
    w, h = 35, 22
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    box = (wide(2) * S, S * 1, wide(30) * S, S * 19)
    glossy_oval(d, box)
    cx, cy, r = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2 + S // 2, S * 5
    lw = int(S * 2.2)
    if symbol == "cross":
        d.line((cx - r, cy - r, cx + r, cy + r), fill=colour, width=lw)
        d.line((cx - r, cy + r, cx + r, cy - r), fill=colour, width=lw)
    elif symbol == "square":
        d.rectangle((cx - r, cy - r, cx + r, cy + r), outline=colour, width=lw)
    else:
        f = ImageFont.truetype(BOLD, 13 * S)
        l, t, rr, b = d.textbbox((0, 0), symbol, font=f)
        d.text((cx - (l + rr) / 2, cy - (t + b) / 2), symbol, font=f, fill=colour,
               stroke_width=S // 2, stroke_fill=(10, 10, 14, 255))
    return finish(img, w, h)


def labelled_bar(text, w, h, font_px):
    """A grey shoulder button with a white label (like the originals' Z / L / R)."""
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((wide(1) * S, S, wide(w - 3) * S, (h - 2) * S), radius=S * 3, fill=(64, 66, 76, 255),
                        outline=(160, 164, 176, 255), width=S)
    d.rounded_rectangle((wide(2) * S, S * 2, wide(w - 4) * S, (h // 2) * S), radius=S * 2, fill=(96, 98, 110, 255))
    f = ImageFont.truetype(BOLD, font_px * S)
    tw = d.textlength(text, font=f)
    tx, ty = (wide(w - 2) * S - tw) / 2, (h - 2) * S / 2 - font_px * S * 0.62
    d.text((tx, ty), text, font=f, fill=(255, 255, 255, 255), stroke_width=S // 2, stroke_fill=(20, 20, 24, 255))
    return finish(img, w, h)


def stick(direction):
    """Right stick pushed in a direction: a dark stick top with an arrow."""
    w, h = 28, 19
    img = canvas(w, h)
    d = ImageDraw.Draw(img)
    box = (wide(2) * S, S * 1, wide(24) * S, S * 16)
    glossy_oval(d, box)
    cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2 + S * 0.5
    r = S * 4.2
    pts = {"up": [(cx, cy - r), (cx - r, cy + r * 0.6), (cx + r, cy + r * 0.6)],
           "dn": [(cx, cy + r), (cx - r, cy - r * 0.6), (cx + r, cy - r * 0.6)],
           "lf": [(cx - r * 1.2, cy), (cx + r * 0.7, cy - r), (cx + r * 0.7, cy + r)],
           "rt": [(cx + r * 1.2, cy), (cx - r * 0.7, cy - r), (cx - r * 0.7, cy + r)]}[direction]
    d.polygon(pts, fill=(235, 238, 245, 255))
    return finish(img, w, h)


# glyph -> (game file it stands in for, origin)
GLYPHS = {"a": ("n64bttna", (0, 12)), "b": ("n64bttnb", (0, 12)), "z": ("n64bttnz", (0, 9)),
          "l": ("n64bttnshl", (0, 12)), "r": ("n64bttnshr", (0, 12)),
          "up": ("n64bttnup", (0, 12)), "dn": ("n64bttndn", (0, 12)), "lf": ("n64bttnlf", (0, 12)), "rt": ("n64bttnrt", (0, 12))}


def family(a, b, z, l, r):
    return {"a": a, "b": b, "z": labelled_bar(z, 29, 15, 9), "l": labelled_bar(l, 65, 13, 9), "r": labelled_bar(r, 65, 13, 9)}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    PREVIEW.mkdir(parents=True, exist_ok=True)
    SHEET.parent.mkdir(parents=True, exist_ok=True)
    fams = {
        "ps": family(face_button("cross", PS_BLUE), face_button("square", PS_PINK), "L2", "L1", "R2"),
        "xb": family(face_button("A", XB_GREEN), face_button("X", XB_BLUE), "LT", "LB", "RT"),
        "sw": family(face_button("B", WHITE), face_button("Y", WHITE), "ZL", "L", "ZR"),
        "rs": {d: stick(d) for d in ("up", "dn", "lf", "rt")},
    }
    n = 0
    for fam, icons in fams.items():
        for g, img in icons.items():
            ox, oy = GLYPHS[g][1]
            (OUT / f"{fam}_{g}.wms").write_bytes(wms.encode_type19(img, 4, ox, oy))
            img.save(PREVIEW / f"{fam}_{g}.png")
            n += 1

    # Review sheet: one row per family as the game shows them (AX-narrowed), 3x, on the game's
    # dark panel colour, with the N64 button each stands for.
    labels = {"ps": "PlayStation", "xb": "Xbox", "sw": "Switch (by position)", "rs": "Right stick (all three) for C"}
    order = ["a", "b", "z", "l", "r", "up", "dn", "lf", "rt"]
    cell, label_w, row_h = 64, 210, 34
    sheet = Image.new("RGBA", (label_w + cell * 9 + 30, 26 + row_h * len(fams)), (14, 16, 30, 255))
    d = ImageDraw.Draw(sheet)
    small = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 11)
    for i, g in enumerate(order):
        d.text((label_w + i * cell + 4, 6), "N64 " + {"a": "A", "b": "B", "z": "Z", "l": "L", "r": "R"}.get(g, "C-" + g),
               font=small, fill=(255, 220, 90, 255))
    for row, (fam, icons) in enumerate(fams.items()):
        y = 26 + row * row_h
        d.text((6, y + 9), labels[fam], font=small, fill=(230, 230, 240, 255))
        for i, g in enumerate(order):
            if g not in icons:
                continue
            img = icons[g]
            shown = img.resize((wide(img.width), img.height), Image.LANCZOS)
            sheet.alpha_composite(shown, (label_w + i * cell + 4, y + 4))
    sheet = sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST)
    sheet.save(SHEET)
    print(f"wrote {n} icons to {OUT.relative_to(ROOT)} (review sheet {SHEET.relative_to(ROOT)})")


if __name__ == "__main__":
    main()
