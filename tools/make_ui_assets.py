#!/usr/bin/env python3
"""Draw the PNG kit for the Blitz '27 house style: the launcher, settings overlay, prompts and toasts.

Writes docs/design/redesign/ui/<name>.png at 1x (sizes in px at 960x720) plus <name>@2x.png and <name>@3x.png
(1080p draws the @2x scaled down, 4K draws the @3x). Backgrounds are 1920x1080, with an @2x at 3840x2160.
docs/design/redesign/ui/README.md lists each file's size and 9-slice insets; the design canvas explains where
each one goes. Everything here is our own abstract artwork (shapes, gradients and lines), except the lockup,
which is the team's art (icons/source/lockup_27.png). Needs Pillow (.venv).
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "design" / "redesign" / "ui"
SS = 4                       # supersampling for anti-aliased edges
SCALES = (1, 2, 3)
LOCKUP_SRC = ROOT / "icons" / "source" / "lockup_27.png"

GLASS = (6, 18, 44, 246)  # review S2: at most ~4% of the game shows through
RIM = (143, 239, 245, 178)
RIM_SOFT = (143, 239, 245, 61)
GRID = (47, 179, 179, 46)
OUTLINE = (6, 10, 26, 230)
CHROME = [(0.00, (250, 252, 255)), (0.38, (176, 188, 208)), (0.50, (70, 82, 108)),
          (0.60, (226, 233, 244)), (1.00, (120, 132, 156))]


# ---- helpers -----------------------------------------------------------------------------------------------

def grad(size, stops, horizontal=False):
    """A linear gradient (RGBA). stops: [(t, (r, g, b[, a])), ...] with t in 0..1."""
    w, h = size
    n = w if horizontal else h
    px = []
    for i in range(n):
        t = i / max(1, n - 1)
        for k in range(len(stops) - 1):
            (t0, c0), (t1, c1) = stops[k], stops[k + 1]
            if t0 <= t <= t1:
                u = (t - t0) / (t1 - t0) if t1 > t0 else 0
                c0 = tuple(c0) + (255,) * (4 - len(c0))
                c1 = tuple(c1) + (255,) * (4 - len(c1))
                px.append(tuple(round(c0[j] + (c1[j] - c0[j]) * u) for j in range(4)))
                break
    line = Image.new("RGBA", (n, 1) if horizontal else (1, n))
    line.putdata(px)
    return line.resize((w, h), Image.NEAREST)


def octagon(x0, y0, x1, y1, c, corners="1111"):
    """Points of a rectangle with 45-degree chamfers of size c on the corners flagged tl, tr, br, bl."""
    tl, tr, br, bl = (c if f == "1" else 0 for f in corners)
    return [(x0 + tl, y0), (x1 - tr, y0), (x1, y0 + tr), (x1, y1 - br), (x1 - br, y1), (x0 + bl, y1),
            (x0, y1 - bl), (x0, y0 + tl)]


def inset(c, d):
    """The chamfer size of an octagon inset by d."""
    return max(0.0, c - d * (math.sqrt(2) - 1))


def mask(size, polys):
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    for p in polys:
        d.polygon(p, fill=255)
    return m


def fill(img, m, colour):
    layer = colour if isinstance(colour, Image.Image) else Image.new("RGBA", img.size, colour)
    img.alpha_composite(Image.composite(layer, Image.new("RGBA", img.size, (0, 0, 0, 0)), m))


def finish(big, w, h):
    """Supersampled RGBA down to w x h, with premultiplied alpha so edges don't go dark."""
    return big.convert("RGBa").resize((w, h), Image.LANCZOS).convert("RGBA")


def save(img, name, scale):
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / (name + ("" if scale == 1 else f"@{scale}x") + ".png")
    img.save(path, optimize=True)
    return path


def kit(name, w, h, draw, scales=SCALES, ss=SS):
    """draw(img, u) paints at u px per design px on an image of (w*u, h*u)."""
    for s in scales:
        u = s * ss
        big = Image.new("RGBA", (w * u, h * u), (0, 0, 0, 0))
        draw(big, u)
        save(finish(big, w * s, h * s) if ss > 1 else big, name, s)


# ---- panels ------------------------------------------------------------------------------------------------

def grid_tile(img, u):
    # One 32 px cell: a line along the top and the left. Tiled, it makes the panel grid.
    d = ImageDraw.Draw(img)
    lw = max(1, u)
    d.rectangle((0, 0, img.width - 1, lw - 1), fill=GRID)
    d.rectangle((0, lw, lw - 1, img.height - 1), fill=GRID)


def panel_frame(img, u, rim=RIM, glass=GLASS, c=8, t=2):
    W, H = img.size
    fill(img, mask(img.size, [octagon(0, 0, W, H, c * u)]), rim)
    k = t * u
    # Clear the inside first, then paint glass, so the rim keeps its own alpha.
    inner = mask(img.size, [octagon(k, k, W - k, H - k, inset(c, t) * u)])
    img.paste((0, 0, 0, 0), (0, 0), inner)
    fill(img, inner, glass)


def chrome_corner(img, u, flip=""):
    # An L bracket with a chamfered outer corner, steel gradient, dark keyline. Drawn top-left, then flipped.
    L, t, k = 20, 5, 7
    a = k + t * math.sqrt(2) - t
    shape = [(0, L), (0, k), (k, 0), (L, 0), (L, t), (a, t), (t, a), (t, L)]
    pts = [(x * u, y * u) for x, y in shape]
    m = mask(img.size, [pts])
    if "h" in flip:
        m = m.transpose(Image.FLIP_LEFT_RIGHT)
    if "v" in flip:
        m = m.transpose(Image.FLIP_TOP_BOTTOM)
    fill(img, m.filter(ImageFilter.MaxFilter(2 * (u // 2) + 1)), OUTLINE)
    fill(img, m, grad(img.size, CHROME))


def header_bevel(img, u):
    # The play-select style header: a steel frame with chamfered corners around a purple-to-navy plate.
    W, H = img.size
    c, t = 14, 5
    fill(img, mask(img.size, [octagon(0, 0, W, H, c * u)]), OUTLINE)
    fill(img, mask(img.size, [octagon(u, u, W - u, H - u, inset(c, 1) * u)]), grad(img.size, CHROME))
    k = t * u
    plate = mask(img.size, [octagon(k, k, W - k, H - k, inset(c, t) * u)])
    img.paste((0, 0, 0, 0), (0, 0), plate)
    fill(img, plate, grad(img.size, [(0, (63, 10, 95)), (0.55, (24, 20, 78)), (1, (10, 22, 56))]))
    k2 = (t + 1) * u
    line = mask(img.size, [octagon(k, k, W - k, H - k, inset(c, t) * u)])
    inner = mask(img.size, [octagon(k2, k2, W - k2, H - k2, inset(c, t + 1) * u)])
    ring = Image.composite(Image.new("L", img.size, 255), Image.new("L", img.size, 0), line)
    ring.paste(0, (0, 0), inner)
    fill(img, ring, (143, 239, 245, 120))


def selector_cap(img, u, right=False):
    # The chrome "[" that closes each end of the focused row. 9-slice vertically: 14 px top and bottom.
    W, H = img.size
    t, k, arm = 4, 6, 12
    a = k + t * math.sqrt(2) - t
    top = [(0, k), (k, 0), (arm, 0), (arm, t), (a, t), (t, a)]
    bot = [(0, 48 - k), (t, 48 - a), (a, 48 - t), (arm, 48 - t), (arm, 48), (k, 48)]
    bar = [(0, k), (t, a), (t, 48 - a), (0, 48 - k)]
    pts = [[(x * u, y * u) for x, y in p] for p in (top, bot, bar)]
    m = mask(img.size, pts)
    if right:
        m = m.transpose(Image.FLIP_LEFT_RIGHT)
    fill(img, m.filter(ImageFilter.MaxFilter(2 * (u // 2) + 1)), OUTLINE)
    fill(img, m, grad(img.size, CHROME))


def segment(img, u, on=True, colour=(69, 224, 74)):
    # A slanted rating block, like team select's bars. 14 x 24, leaning right.
    W, H = img.size
    pts = [(5 * u, 0), (W, 0), (W - 5 * u, H), (0, H)]
    m = mask(img.size, [pts])
    if on:
        hi = tuple(min(255, v + 90) for v in colour)
        lo = tuple(int(v * 0.62) for v in colour)
        fill(img, m, grad(img.size, [(0, hi), (0.35, colour), (1, lo)]))
    else:
        fill(img, m, (19, 37, 74, 255))
        inner = [(5 * u + 2 * u, 1.5 * u), (W - 1.5 * u, 1.5 * u), (W - 5 * u - 1.5 * u, H - 1.5 * u), (1.5 * u, H - 1.5 * u)]
        ring = m.copy()
        ring.paste(0, (0, 0), mask(img.size, [inner]))
        fill(img, ring, RIM_SOFT)


def flame_strip(img, u):
    # Bottom-anchored flame glow, stretched across the launcher's floor.
    W, H = img.size
    img.alpha_composite(grad((W, H), [(0, (255, 208, 0, 0)), (0.45, (255, 196, 0, 70)), (0.75, (255, 140, 0, 190)),
                                      (1, (255, 80, 0, 255))]))


def rule(img, u, stops):
    img.alpha_composite(grad(img.size, stops, horizontal=True))


# ---- backgrounds -------------------------------------------------------------------------------------------

def band(size, pts, width, colour, blur):
    """A soft curved band through pts (a smooth polyline), as an RGBA layer."""
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.line(pts, fill=colour, width=int(width), joint="curve")
    return layer.filter(ImageFilter.GaussianBlur(blur)) if blur else layer


def bezier(p0, p1, p2, p3, n=90):
    out = []
    for i in range(n + 1):
        t = i / n
        a, b, c, d = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
        out.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0], a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))
    return out


def bg_title(scale):
    W, H = 1920 * scale, 1080 * scale
    s = scale
    img = grad((W, H), [(0, (2, 7, 18)), (0.55, (7, 15, 34)), (1, (12, 10, 40))])
    # Purple swooshes from the lower left up and off the top right.
    img.alpha_composite(band((W, H), bezier((-300 * s, 1150 * s), (500 * s, 760 * s), (1150 * s, 420 * s), (2250 * s, -160 * s)),
                             300 * s, (63, 10, 95, 205), 70 * s))
    img.alpha_composite(band((W, H), bezier((-200 * s, 1240 * s), (700 * s, 900 * s), (1350 * s, 560 * s), (2250 * s, 80 * s)),
                             70 * s, (118, 34, 160, 120), 26 * s))
    img.alpha_composite(band((W, H), bezier((-100 * s, 980 * s), (520 * s, 700 * s), (1250 * s, 330 * s), (2100 * s, -60 * s)),
                             5 * s, (95, 220, 230, 90), 2 * s))
    # A stadium floor: a cyan perspective grid in the lower third, fading toward the horizon.
    floor = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(floor)
    hy, vx = 760 * s, W / 2
    for i in range(-16, 17):
        x = vx + i * 150 * s
        d.line([(vx + i * 14 * s, hy), (x, H)], fill=(47, 179, 179, 110), width=max(1, s))
    y, step = hy, 10 * s
    while y < H:
        d.line([(0, y), (W, y)], fill=(47, 179, 179, 110), width=max(1, s))
        step *= 1.32
        y += step
    fade = grad((W, H), [(0, (0, 0, 0, 0)), (hy / H, (0, 0, 0, 0)), (1, (0, 0, 0, 255))]).getchannel("A")
    floor.putalpha(Image.composite(floor.getchannel("A"), Image.new("L", (W, H), 0), fade))
    img.alpha_composite(floor)
    # Flame along the floor: orange at the edge, yellow at its heart, brightest in the middle.
    fh = 150 * s
    flame = grad((W, fh), [(0, (255, 208, 0, 0)), (0.55, (255, 176, 0, 80)), (0.85, (255, 120, 0, 190)), (1, (255, 84, 0, 235))])
    heart = Image.new("L", (W, fh), 0)
    ImageDraw.Draw(heart).ellipse((-W * 0.1, fh * 0.35, W * 1.1, fh * 1.9), fill=255)
    heart = heart.filter(ImageFilter.GaussianBlur(60 * s))
    flame.putalpha(Image.composite(flame.getchannel("A"), Image.eval(flame.getchannel("A"), lambda v: v // 2), heart))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    glow.alpha_composite(flame, (0, H - fh))
    img.alpha_composite(glow)
    # Darken the corners a little so the frame reads as a TV picture.
    vign = Image.new("L", (W, H), 0)
    ImageDraw.Draw(vign).ellipse((-W * 0.25, -H * 0.35, W * 1.25, H * 1.35), fill=255)
    vign = vign.filter(ImageFilter.GaussianBlur(160 * s))
    dark = Image.new("RGBA", (W, H), (2, 7, 18, 200))
    dark.putalpha(Image.eval(vign, lambda v: int((255 - v) * 0.8)))
    img.alpha_composite(dark)
    return img.convert("RGB")


def bg_overlay_swoosh(scale):
    # Laid over ModalOverlay behind the settings panel: one purple swoosh across the top right.
    W, H = 1280 * scale, 720 * scale
    s = scale
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    img.alpha_composite(band((W, H), bezier((560 * s, -40 * s), (900 * s, 120 * s), (1150 * s, 260 * s), (1400 * s, 300 * s)),
                             150 * s, (63, 10, 95, 150), 50 * s))
    img.alpha_composite(band((W, H), bezier((-120 * s, 760 * s), (240 * s, 640 * s), (420 * s, 660 * s), (720 * s, 800 * s)),
                             110 * s, (63, 10, 95, 110), 46 * s))
    return img


# ---- logo lockup -------------------------------------------------------------------------------------------

def lockup(scale):
    # The team's "BLITZ '27" lockup (icons/source/lockup_27.png), trimmed to its solid edge plus a little room,
    # drawn 48 px tall at 1x. It's 8:1, so it suits 20-48 px tall placements; under 20 px use the app icon.
    im = Image.open(LOCKUP_SRC).convert("RGBA")
    box = im.getchannel("A").point(lambda a: 255 if a > 40 else 0).getbbox()
    pad = round((box[3] - box[1]) * 0.04)
    im = im.crop((max(0, box[0] - pad), max(0, box[1] - pad), min(im.width, box[2] + pad), min(im.height, box[3] + pad)))
    h = 48 * scale
    return finish(im, round(im.width * h / im.height), h)


# ---- main --------------------------------------------------------------------------------------------------

def main():
    kit("grid_tile", 32, 32, grid_tile, ss=1)
    kit("panel_frame", 48, 48, panel_frame)
    kit("panel_frame_hot", 48, 48, lambda i, u: panel_frame(i, u, rim=(255, 225, 74, 255), t=3))
    kit("panel_frame_p1", 48, 48, lambda i, u: panel_frame(i, u, rim=(58, 123, 255, 255)))
    kit("panel_frame_p2", 48, 48, lambda i, u: panel_frame(i, u, rim=(255, 77, 77, 255)))
    kit("panel_frame_p3", 48, 48, lambda i, u: panel_frame(i, u, rim=(47, 211, 107, 255)))
    kit("panel_frame_p4", 48, 48, lambda i, u: panel_frame(i, u, rim=(255, 210, 63, 255)))
    for name, flip in (("tl", ""), ("tr", "h"), ("bl", "v"), ("br", "hv")):
        kit("chrome_corner_" + name, 20, 20, lambda i, u, f=flip: chrome_corner(i, u, f))
    kit("header_bevel", 96, 64, header_bevel)
    kit("selector_cap_l", 12, 48, selector_cap)
    kit("selector_cap_r", 12, 48, lambda i, u: selector_cap(i, u, right=True))
    kit("seg_on", 14, 24, segment)
    kit("seg_off", 14, 24, lambda i, u: segment(i, u, on=False))
    kit("flame_strip", 16, 128, flame_strip, ss=1)
    kit("rule_chrome", 256, 4, lambda i, u: rule(i, u, [(0, (250, 252, 255)), (0.3, (150, 162, 186)), (0.55, (226, 233, 244)),
                                                       (0.8, (90, 102, 128, 160)), (1, (90, 102, 128, 0))]), ss=1)
    kit("rule_chrome_rev", 256, 4, lambda i, u: rule(i, u, [(0, (90, 102, 128, 0)), (0.2, (90, 102, 128, 160)), (0.45, (226, 233, 244)),
                                                           (0.7, (150, 162, 186)), (1, (250, 252, 255))]), ss=1)
    kit("rule_flame", 256, 4, lambda i, u: rule(i, u, [(0, (255, 80, 0)), (0.45, (255, 170, 0)), (0.75, (255, 208, 0, 200)),
                                                      (1, (255, 208, 0, 0))]), ss=1)
    for s in (1, 2):
        save(bg_title(s), "bg_title", s)
        save(bg_overlay_swoosh(s), "bg_overlay_swoosh", s)
    for s in SCALES:
        save(lockup(s), "logo27_lockup", s)
    print("wrote", len(list(OUT.glob("*.png"))), "files to", OUT.relative_to(ROOT))


if __name__ == "__main__":
    main()
