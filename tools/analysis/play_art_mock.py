#!/usr/bin/env python3
"""Mock of pre-snap play art: a play's decoded routes painted onto a screenshot at the line.

Analysis only (docs/notes/PLAY_ART.md). The field-to-picture mapping is a homography fitted by hand to one frame
(the trailer's bomb.mp4 at 0.05 s, 1920x1080: Da Bomb, 4th and goal); another frame needs its own four points.

  play_art_mock.py FRAME.png OUT.png --play "Da Bomb" [--style full|hot] [--rom build/baserom.z64]
"""
import argparse
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from play_routes import ROOT, UNITS_PER_YARD, Rom, load_plays, trace  # noqa: E402

# Field (X across in yards, Y yards from the goal line) -> picture, fitted to that frame's sidelines and 5-yard lines.
# The across scale is calibrated on the players (the field is about 47 yards wide in game units, not 53.3).
HALF_WIDTH = 23.5
FIT_SRC = [(-HALF_WIDTH, 0), (HALF_WIDTH, 0), (-HALF_WIDTH, 20), (HALF_WIDTH, 20)]
FIT_DST = [(277, 232), (1495, 232), (-18, 418), (1740, 418)]
LOS_Y, BALL_X = 31.7, 2.7      # the orange line (y = 630) and the center's spot (x = 965)

# Receiver Buttons colours, left to right on screen (triangle, R1, circle on a PlayStation pad).
BUTTON_COLOURS = [(70, 211, 154), (245, 197, 66), (240, 82, 79)]
BUTTON_LABELS = ["△", "R1", "○"]


def solve(a, b):
    n = len(a)
    m = [r[:] + [v] for r, v in zip(a, b)]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(m[r][i]))
        m[i], m[p] = m[p], m[i]
        for r in range(n):
            if r != i:
                f = m[r][i] / m[i][i]
                m[r] = [x - f * y for x, y in zip(m[r], m[i])]
    return [m[i][n] / m[i][i] for i in range(n)]


def homography(src, dst):
    a, b = [], []
    for (x, y), (u, v) in zip(src, dst):
        a.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); b.append(u)
        a.append([0, 0, 0, x, y, 1, -v * x, -v * y]); b.append(v)
    h = solve(a, b) + [1]

    def f(x, y):
        w = h[6] * x + h[7] * y + 1
        return ((h[0] * x + h[1] * y + h[2]) / w, (h[3] * x + h[4] * y + h[5]) / w)
    return f


FIELD = homography(FIT_SRC, FIT_DST)


def to_px(d, l):
    """Offense frame (D downfield from the line, L across, + = offense's right) -> picture."""
    return FIELD(BALL_X + l, LOS_Y - d)


def clip_path(pts, max_len, max_depth):
    """The route as the play art shows it: the scripted path, cut at a length and a depth."""
    out = [pts[0]]
    left = max_len
    for (d0, l0), (d1, l1) in zip(pts, pts[1:]):
        seg = math.hypot(d1 - d0, l1 - l0)
        if seg <= 0:
            continue
        t = min(1.0, left / seg)
        if d1 > max_depth and d1 != d0:
            t = min(t, max(0.0, (max_depth - d0) / (d1 - d0)))
        out.append((d0 + (d1 - d0) * t, l0 + (l1 - l0) * t))
        left -= seg * t
        if t < 1.0 or left <= 0:
            break
    return out


def ribbon(draw, pts, colour, width_yd, alpha):
    """A route painted on the turf: each leg a quad in field space, joints rounded, an arrowhead at the end."""
    fill = colour + (alpha,)
    edge = (10, 20, 10, int(alpha * 0.8))
    hw = width_yd / 2
    polys = []
    for (d0, l0), (d1, l1) in zip(pts, pts[1:]):
        n = math.hypot(d1 - d0, l1 - l0)
        if n < 1e-6:
            continue
        nd, nl = -(l1 - l0) / n * hw, (d1 - d0) / n * hw
        polys.append([to_px(d0 + nd, l0 + nl), to_px(d1 + nd, l1 + nl), to_px(d1 - nd, l1 - nl), to_px(d0 - nd, l0 - nl)])
    joints = []
    for d, l in pts[1:-1]:
        joints.append([to_px(d + hw * math.cos(a), l + hw * math.sin(a)) for a in [k * math.pi / 8 for k in range(16)]])
    # arrowhead
    (d0, l0), (d1, l1) = pts[-2], pts[-1]
    n = math.hypot(d1 - d0, l1 - l0) or 1
    ud, ul = (d1 - d0) / n, (l1 - l0) / n
    head = [to_px(d1 + ud * 1.6, l1 + ul * 1.6), to_px(d1 - ul * 1.3, l1 + ud * 1.3), to_px(d1 + ul * 1.3, l1 - ud * 1.3)]
    for p in polys + joints + [head]:
        draw.polygon(p, outline=edge, width=5)
    for p in polys + joints + [head]:
        draw.polygon(p, fill=fill)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frame")
    ap.add_argument("out")
    ap.add_argument("--play", default="Da Bomb")
    ap.add_argument("--style", choices=["full", "hot"], default="full")
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    a = ap.parse_args()

    play = {p["name"]: p for p in load_plays(Rom(a.rom))}[a.play]
    base = Image.open(a.frame).convert("RGBA")
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)

    receivers = []
    for pl in play["players"]:
        if pl["slot"] < 4:
            continue
        d0, l0 = pl["x"] / UNITS_PER_YARD, pl["z"] / UNITS_PER_YARD
        pts, _ = trace((d0, l0), [tuple(o) for o in pl["ops"]], flipped=l0 < 0)
        receivers.append((l0, pts))
    receivers.sort()  # left to right as they line up, the Receiver Buttons order

    for k, (l0, pts) in enumerate(receivers):
        if a.style == "full":
            path = clip_path(pts, max_len=40, max_depth=27)
            ribbon(draw, path, BUTTON_COLOURS[k], 0.9, 170)
        else:
            path = clip_path(pts, max_len=12, max_depth=12)
            ribbon(draw, path, BUTTON_COLOURS[k], 0.7, 200)

    out = Image.alpha_composite(base, layer)
    d = ImageDraw.Draw(out)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 30)
        small = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 24)
    except OSError:
        font = small = ImageFont.load_default()
    for k, (l0, pts) in enumerate(receivers):
        x, y = to_px(pts[0][0] - 1.6, pts[0][1])
        d.ellipse([x - 22, y - 22, x + 22, y + 22], fill=(15, 15, 25, 230), outline=BUTTON_COLOURS[k] + (255,), width=4)
        if BUTTON_LABELS[k] == "△":  # Arial has no triangle: draw it
            d.polygon([(x, y - 11), (x - 11, y + 8), (x + 11, y + 8)], outline=BUTTON_COLOURS[k] + (255,), width=3)
        else:
            d.text((x, y), BUTTON_LABELS[k], fill=BUTTON_COLOURS[k] + (255,), font=small, anchor="mm")
    label = f"PLAY ART MOCK: {a.play.upper()} ({'routes' if a.style == 'full' else 'first break only'}), decoded from the ROM"
    d.rectangle([40, 1000, 40 + 16 * len(label) + 30, 1050], fill=(0, 0, 0, 180))
    d.text((55, 1025), label, fill=(255, 255, 255, 255), font=font, anchor="lm")
    out.convert("RGB").save(a.out)


if __name__ == "__main__":
    main()
