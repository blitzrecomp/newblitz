#!/usr/bin/env python3
"""Make the uniform mannequin: one player's body and helmet, with the game's UVs, from an RDRAM dump of your game.

  .venv/bin/python tools/make_mannequin.py DUMP.bin [--dlist ADDR] [--side 0|1] [--slot N] [--out build/uniform_mannequin]

DUMP.bin is a `tools/live.sh dump` (its .txt names the display list; --dlist overrides). The dump is walked with
tools/gbi_dump.py (the same RSP/RDP model), and every triangle drawn with one of a side's uniform textures is
kept with its texture coordinates. Which texture is which part comes from the game's own layout: main func_80256580
loads a side's ten parts into fixed buffers (D_802BE4B0 + side * 0x60E0; see PART_BUFFERS) and builds one jersey per
player slot from the jersey and number parts. The player kept is the biggest on screen whose textures are that
side's (or --side / --slot).

For each draw the mannequin stores the RDP tile (wrap, mirror, clamp, mask, shift, filter) and a lookup from each
texel the tile can address to the uniform part pixel it shows, so tools/uniform_preview.py can wrap any uniform set
exactly as the game does. Textures that aren't uniform parts (skin, gloves, the ball) are kept as decoded texels.

The output (build/uniform_mannequin/mannequin.json, check.png drawn from the dump's own camera) is derived from your
ROM, so it stays in build/ and is never committed.
"""
import argparse
import json
import math
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gbi_dump as g  # noqa: E402  (it takes tools/ off sys.path: tools/dis.py shadows the stdlib's)
sys.path.append(str(ROOT / "tools"))

# main func_80256580: a side's uniform buffers, as offsets from D_802BE4B0 + side * 0x60E0 (physical 0x2BE4B0).
SIDE_BASE, SIDE_STRIDE = 0x2BE4B0, 0x60E0
JERSEY_SLOTS = 15                     # one jersey per player slot (0-14), 0x400 apart, each with its numbers drawn in
# part -> (pixel offset, width, height, bits per texel, palette offset or None)
PART_BUFFERS = {
    "helmet_side": (0x54E0, 32, 32, 16, None),
    "helmet_stripe_front": (0x5CE0, 32, 8, 16, None),
    "helmet_stripe_back": (0x5EE0, 32, 8, 16, None),
    "facemask": (0x4CA0, 32, 32, 4, 0x54A0),
    "shoulder": (0x4000, 32, 16, 4, 0x4C40),
    "jersey": (0x0000, 40, 40, 4, 0x4C00),      # slot 0; slot k at + k * 0x400
    "pant_stripe": (0x4400, 8, 2, 4, 0x4C60),
    "pants": (0x3C00, 32, 32, 4, 0x4C20),
    "socks": (0x4800, 8, 2, 4, 0x4C80),
    "numbers": (0x50A0, 128, 16, 4, 0x54C0),
}
PART_IDS = list(PART_BUFFERS)


def regions(side):
    """(start, end, part, slot, width, bpp) for one side's buffers."""
    base = SIDE_BASE + side * SIDE_STRIDE
    out = []
    for part, (off, w, h, bpp, _) in PART_BUFFERS.items():
        n = w * h * bpp // 8
        if part == "jersey":
            out += [(base + off + k * 0x400, base + off + k * 0x400 + n, part, k, w, bpp) for k in range(JERSEY_SLOTS)]
        else:
            out.append((base + off, base + off + n, part, None, w, bpp))
    return out


class Walker(g.Walker):
    """gbi_dump's walker, also keeping which RDRAM bytes each TMEM word holds and each draw's matrices."""

    def __init__(self, mem, args):
        super().__init__(mem, args)
        self.owner = [-1] * 512   # TMEM word -> physical RDRAM address of its first byte (-1: none / TLUT)

    def record_load(self, kind, addr, tile_idx, params, start, nbytes):
        st = self.st
        t = st.tiles[tile_idx]
        fmt, siz, width, timg = st.timg
        if kind == "LOADBLOCK":
            for i in range((nbytes + 7) // 8):
                self.owner[(t.tmem + i) & 0x1FF] = start + 8 * i
        elif kind == "LOADTILE":
            bpp = g.SIZ_BITS[siz]
            x0, y0, x1, y1 = (int(p) for p in params)
            row = ((x1 - x0 + 1) * bpp + 7) // 8
            for r in range(y1 - y0 + 1):
                src = timg + ((y0 + r) * width + x0) * bpp // 8
                for i in range((row + 7) // 8):
                    self.owner[(t.tmem + r * t.line + i) & 0x1FF] = src + 8 * i
        else:
            for i in range(nbytes // 2):
                self.owner[(t.tmem + i) & 0x1FF] = -1
        super().record_load(kind, addr, tile_idx, params, start, nbytes)

    def new_draw(self, addr, kind, tile_override=None):
        d = super().new_draw(addr, kind, tile_override)
        d.owner = list(self.owner)
        d.proj = self.st.proj
        d.mv = self.st.mv
        return d


def invert4(m):
    """Inverse of a 4x4 matrix (rows), by Gauss-Jordan."""
    a = [list(map(float, r)) + [1.0 if i == j else 0.0 for j in range(4)] for i, r in enumerate(m)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        f = a[c][c]
        a[c] = [x / f for x in a[c]]
        for r in range(4):
            if r != c:
                k = a[r][c]
                a[r] = [x - k * y for x, y in zip(a[r], a[c])]
    return [r[4:] for r in a]


def tile_range(tl):
    """How many texels the tile can address on each axis (what tile_coord can return)."""
    w = (1 << tl.masks) if tl.masks else (tl.lrs >> 2) - (tl.uls >> 2) + 1
    h = (1 << tl.maskt) if tl.maskt else (tl.lrt >> 2) - (tl.ult >> 2) + 1
    return max(1, min(w, 1024)), max(1, min(h, 1024))


def texel_source(d, tl, s, t, regs):
    """The uniform part pixel tile texel (s, t) shows: (part, slot, x, y), or None."""
    bpp = g.SIZ_BITS[tl.siz]
    a = tl.tmem * 8 + t * tl.line * 8 + (s * bpp) // 8
    word = (a >> 3) & 0x1FF
    src = d.owner[word]
    if src < 0:
        return None
    phys = src + (a & 7)
    for start, end, part, slot, w, rbpp in regs:
        if start <= phys < end and rbpp == bpp:
            px = (phys - start) * 8 // bpp + ((s & 1) if bpp == 4 else 0)
            return part, slot, px % w, px // w
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dump", type=Path)
    ap.add_argument("--dlist", help="display list address (hex); default from the dump's .txt")
    ap.add_argument("--side", type=int, choices=[0, 1], help="the side whose player to keep (default: the biggest player's)")
    ap.add_argument("--slot", type=int, help="the player slot (jersey 0-14) to keep (default: the biggest on screen)")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "uniform_mannequin")
    ap.add_argument("--name", default="mannequin", help="output name: <out>/<name>.json (default mannequin)")
    a = ap.parse_args(argv)
    txt = a.dump.with_suffix(".txt")
    dl = int(a.dlist, 16) if a.dlist else int(re.search(r"dlist=([0-9A-Fa-f]+)", txt.read_text()).group(1), 16)
    args = argparse.Namespace(cmds=False, follow=True, cull="eval", branch_z="eval", max_cmds=400000,
                              vertices=False, matrices=False)
    mem = g.Rdram(a.dump)
    w = Walker(mem, args)
    w.run(dl)

    regs = {side: regions(side) for side in (0, 1)}
    # Each textured triangle draw: which side's uniform it samples (by its tile's first texel).
    draws = []
    for d in w.draws:
        if d.kind != "tris" or not d.tri_data or d.proj is None:
            continue
        snap = d.snap
        tl = snap.tiles[snap.tex_tile]
        hit = None
        if snap.tex_on:
            for side in (0, 1):
                sw, sh = tile_range(tl)
                for (s, t) in ((0, 0), (sw // 2, sh // 2), (sw - 1, sh - 1)):
                    src = texel_source(d, tl, s, t, regs[side])
                    if src:
                        hit = (side, src)
                        break
                if hit:
                    break
        draws.append((d, hit))

    # Players: runs of draws between a jersey and the next player's; pick the biggest one on screen.
    def area(ds):
        xs = [b for d, _ in ds if d.screen_bbox for b in (d.screen_bbox[0], d.screen_bbox[2])]
        ys = [b for d, _ in ds if d.screen_bbox for b in (d.screen_bbox[1], d.screen_bbox[3])]
        return (max(xs) - min(xs)) * (max(ys) - min(ys)) if xs else 0, (min(xs), min(ys), max(xs), max(ys)) if xs else None

    jerseys = [(i, hit) for i, (d, hit) in enumerate(draws) if hit and hit[1][0] == "jersey"]
    if not jerseys:
        raise SystemExit("no player in this dump (no draw samples a jersey buffer)")
    candidates = []
    for i, hit in jerseys:
        side, slot = hit[0], hit[1][1]
        if (a.side is not None and side != a.side) or (a.slot is not None and slot != a.slot):
            continue
        # The player's draws: those near the jersey in the list whose matrices sit close in view space.
        d0 = draws[i][0]
        lo, hi = i, i
        while lo > 0 and i - lo < 40 and not (draws[lo - 1][1] and draws[lo - 1][1][1][0] == "jersey"):
            lo -= 1
        while hi + 1 < len(draws) and hi - i < 40 and not (draws[hi + 1][1] and draws[hi + 1][1][1][0] == "jersey"):
            hi += 1
        ex = [draws[k] for k in range(lo, hi + 1)]
        centre = d0.mv[3][:3] if d0.mv else [0, 0, 0]
        near = [x for x in ex if x[0].mv and math.dist(x[0].mv[3][:3], centre) < 260 and (x[1] is None or x[1][0] == side)]
        _, pbox = area([x for x in near if x[1]])
        if pbox:
            mx, my = (pbox[2] - pbox[0]) * 0.6, (pbox[3] - pbox[1]) * 0.6
            inside = lambda b: b and pbox[0] - mx <= (b[0] + b[2]) / 2 <= pbox[2] + mx and pbox[1] - my <= (b[1] + b[3]) / 2 <= pbox[3] + my  # noqa: E731
            near = [x for x in near if x[1] or inside(x[0].screen_bbox)]
        keep = near
        ar, box = area(keep)
        candidates.append((ar, side, slot, keep, box))
    if not candidates:
        raise SystemExit("no player matches --side/--slot")
    candidates.sort(key=lambda c: -c[0])
    _, side, slot, keep, box = candidates[0]

    # View space: clip coordinates through the inverse projection (row vectors, as gbi_dump).
    out_draws, tris = [], []
    for d, hit in keep:
        snap = d.snap
        inv = invert4(d.proj)
        tl = snap.tiles[snap.tex_tile]
        entry = {
            "dl": f"{d.dl_addr:08X}", "geometry": snap.geometry, "othermode_h": snap.othermode_h,
            "othermode_l": snap.othermode_l, "combine": [snap.combine[0], snap.combine[1]],
            "prim": snap.prim, "env": snap.env, "tex_on": snap.tex_on,
            "cull_back": bool(snap.geometry & g.G_CULL_BACK), "cull_front": bool(snap.geometry & g.G_CULL_FRONT),
            "filter": ["POINT", "?", "BILERP", "AVERAGE"][g.bits(snap.othermode_h, 12, 2)],
        }
        if snap.tex_on:
            sw, sh = tile_range(tl)
            entry["tile"] = {k: getattr(tl, k) for k in ("fmt", "siz", "line", "tmem", "palette", "cms", "cmt", "masks",
                                                         "maskt", "shifts", "shiftt", "uls", "ult", "lrs", "lrt")}
            entry["size"] = [sw, sh]
            tlut = g.bits(snap.othermode_h, 14, 2)
            parts, fixed, used = [], [], set()
            for t in range(sh):
                for s in range(sw):
                    src = texel_source(d, tl, s, t, regs[side])
                    if src:
                        part, pslot, x, y = src
                        parts.append([PART_IDS.index(part), x, y])
                        used.add(part if part != "jersey" else f"jersey/{pslot}")
                        fixed.append(0)
                    else:
                        parts.append(None)
                        r, gg, b, al = g.tmem_texel(snap.tmem, tl, s, t, tlut)
                        fixed.append((r << 24) | (gg << 16) | (b << 8) | al)
            entry["texels"] = parts
            entry["fixed"] = fixed
            entry["parts"] = sorted(used)
        for tri in d.tri_data:
            vs = []
            for (cx, cy, cz, cw, s, t, r, gg, b, al) in tri:
                v = [cx * inv[0][j] + cy * inv[1][j] + cz * inv[2][j] + cw * inv[3][j] for j in range(4)]
                vs.append([v[0] / v[3], v[1] / v[3], v[2] / v[3], s / 32.0, t / 32.0, r, gg, b, al])
            tris.append([len(out_draws)] + vs)
        out_draws.append(entry)

    # A body frame: up from the feet to the helmet, front out through the facemask.
    def centroid(pred):
        pts = [v[:3] for tri in tris if pred(out_draws[tri[0]]) for v in tri[1:]]
        return [sum(p[k] for p in pts) / len(pts) for k in range(3)] if pts else None
    helmet = centroid(lambda e: "helmet_side" in e.get("parts", []))
    mask = centroid(lambda e: "facemask" in e.get("parts", []))
    socks = centroid(lambda e: "socks" in e.get("parts", []) or "pant_stripe" in e.get("parts", []))
    if not (helmet and mask):
        raise SystemExit("the chosen player has no helmet or facemask draw; try another dump")
    allv = [v[:3] for tri in tris for v in tri[1:]]
    lowest = min(allv, key=lambda p: p[1])
    feet = socks or lowest
    up = [h - f for h, f in zip(helmet, feet)]
    n = math.sqrt(sum(x * x for x in up))
    up = [x / n for x in up]
    fwd = [m - h for m, h in zip(mask, helmet)]
    k = sum(f * u for f, u in zip(fwd, up))
    fwd = [f - k * u for f, u in zip(fwd, up)]
    n = math.sqrt(sum(x * x for x in fwd))
    fwd = [x / n for x in fwd]
    right = [up[1] * fwd[2] - up[2] * fwd[1], up[2] * fwd[0] - up[0] * fwd[2], up[0] * fwd[1] - up[1] * fwd[0]]
    # The uniform the dump's player wears, read back from the buffers (for the check render and the match proof).
    base = SIDE_BASE + side * SIDE_STRIDE
    dump_uniform = {}
    for part, (off, pw, ph, bpp, paloff) in PART_BUFFERS.items():
        start = base + off + (slot * 0x400 if part == "jersey" else 0)
        raw = mem.read(start, pw * ph * bpp // 8)
        if bpp == 16:
            px = [g.rgba5551(int.from_bytes(raw[2 * i:2 * i + 2], "big")) for i in range(pw * ph)]
        else:
            pal = [mem.u16(base + paloff + 2 * i) for i in range(16)]
            px = [g.rgba5551(pal[(raw[i // 2] >> (4 if i % 2 == 0 else 0)) & 15]) for i in range(pw * ph)]
        dump_uniform[part] = {"w": pw, "h": ph, "rgba": [list(p) for p in px]}
    meta = {
        "dump_uniform": dump_uniform,
        "source": {"dump": a.dump.name, "dlist": f"{dl:08X}", "side": side, "slot": slot,
                   "screen_box": [round(x, 1) for x in box]},
        "made_by": "tools/make_mannequin.py",
        "note": "Derived from the game's own data in your ROM (an RDRAM dump of it running): stays in build/, never committed.",
        "part_ids": PART_IDS,
        "frame": {"origin": helmet, "up": up, "front": fwd, "right": right, "feet": feet},
        "camera": {"proj": keep[0][0].proj, "viewport": keep[0][0].vp},
        "draws": out_draws,
        "tris": tris,
    }
    a.out.mkdir(parents=True, exist_ok=True)
    path = a.out / f"{a.name}.json"
    path.write_text(json.dumps(meta, separators=(",", ":")))
    (a.out / f"{a.name}_SOURCE.txt").write_text(
        f"Made by tools/make_mannequin.py from {a.dump} (display list {dl:08X}), side {side}, slot {slot}.\n"
        f"{len(out_draws)} draws, {len(tris)} triangles; parts: "
        f"{', '.join(sorted({p for e in out_draws for p in e.get('parts', [])}))}.\n"
        "Derived from your ROM: keep it in build/, never commit it.\n")
    print(f"{path}: side {side} slot {slot}, {len(out_draws)} draws, {len(tris)} triangles, screen box {box}")
    for e in out_draws:
        print(f"  draw @{e['dl']}: {', '.join(e.get('parts', [])) or ('untextured' if not e['tex_on'] else 'not a uniform part')}"
              f" {e.get('size', '')} {e['filter']}")
    try:
        import uniform_preview
        uniform_preview.check_image(path, a.out / f"{a.name}_check.png")
        print(f"check render (the dump's camera, the uniform in the dump): {a.out / (a.name + '_check.png')}")
    except ImportError:
        pass


if __name__ == "__main__":
    main()
