#!/usr/bin/env python3
"""Build a league pak (e.g. "Blitz: Pac-12", packs/pac12) from a league config: its teams on NFL base teams,
with fictional players and our own art.

  .venv/bin/python tools/make_league_pak.py [--league pac12] [--rom build/teamplus.z64] [--out DIR] [--sheet PNG]

--league names tools/leagues/<name>.json (or is a path to one): the pack.json header (id, title, version, league
name, modes, ladder) and each team's id, NFL base, names, colours and ratings. Nothing about a league is written
in this file, so one importer serves every league.

A league pak's teams are built on NFL franchises (their CPU plays, skill, uniforms and 3D models); its
images replace the base teams' files by name, which is safe because no NFL team plays while the pak is
active (src/game/content_pack_tp.cpp, docs/BLITZ2026.md "League paks"). Every image here is drawn from
text and shapes: school initials, wordmarks, a plate, a helmet, end zone lettering, in each school's
colours. The ROM is read only for the base teams' file names, sizes and origins, never for pixels.

Players are fictional (college players' names belong to them), generated from a fixed seed.
"""
import argparse
import hashlib
import io
import json
import random
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "announcer"))
import fs  # noqa: E402
import wms  # noqa: E402

FONT = "/System/Library/Fonts/Supplemental/Arial Black.ttf"
BLOCKS, BLOCK = 0x11F17C, 0x220   # team+ team blocks in the ROM

from uniform_handoff import NICKNAMES  # noqa: E402  every franchise's nickname(s) in the ROM's team list

RATING_NAMES = ["passing", "rushing", "linemen", "defense", "special_teams"]


def load_league(name):
    """tools/leagues/<name>.json, or a path to a league config."""
    path = Path(name)
    if not path.suffix:
        path = ROOT / "tools" / "leagues" / f"{name}.json"
    cfg = json.loads(path.read_text())
    ids = [t["id"] for t in cfg["teams"]]
    bases = [t["base"] for t in cfg["teams"]]
    if len(set(ids)) != len(ids):
        raise SystemExit(f"{path}: a team id appears twice")
    if len(set(bases)) != len(bases):
        # The pak's images replace the base team's files by name: two teams on one base overwrite each other's art.
        raise SystemExit(f"{path}: one NFL base per team (the art is keyed by the base's file names)")
    for b in bases:
        if b not in NICKNAMES:
            raise SystemExit(f"{path}: base {b} isn't an NFL franchise id")
    return cfg


def team_ratings(t):
    r = t.get("ratings", [])
    return dict(zip(RATING_NAMES, r)) if isinstance(r, list) else {k: r[k] for k in RATING_NAMES if k in r}

SURNAMES = """Abernathy Acosta Adair Alcott Aldridge Ambrose Ashby Avery Bancroft Barlow Beckett Bellamy Benning Birch
Blackwell Bolden Bowen Bradshaw Brannon Briggs Brock Burgess Calloway Cardenas Carver Castillo Chambers Childers Clement
Colburn Conley Corbin Crane Cutler Dalton Darby Delgado Denham Dorsey Draper Dunham Eastman Ellison Emery Farrow Fenwick
Finch Fleming Forsyth Fowler Gaines Galvan Garrity Gentry Gilmore Goodwin Grady Grimes Hadley Halsey Harlan Hartwell
Hawley Hendrix Holloway Hopper Huxley Ingram Iverson Jarrett Kearney Keller Kendrick Kirby Lacey Langley Larkin Lockett
Lowry Maddox Malone Marlowe Mercer Merritt Monroe Nash Norwood Oakley Osborne Pace Paxton Pickett Prescott Quinlan
Radford Ramsey Redding Rhodes Rowland Rutledge Salazar Sawyer Sexton Shelby Slater Sutton Talley Tatum Thornton Tolliver
Upton Vance Vaughn Waller Walsh Whitfield Wilder Winslow Yancey Yates Zamora Kahale Faleolo Tuiasosopo Mapu Okafor
Adeyemi Nwosu Obi Haskins Bledsoe Tillman Pruitt Batiste Fontenot Thibodeaux Moua Nakamura Takeda Varga Kowalski
Novak Petrakis Rossi Moreno Ibarra Quintero Villalobos""".split()

# Jersey numbers by slot (0-2 line, 3 QB, 4 RB, 5-6 receivers, 7-9 defensive line, 10-14 linebackers and backs).
NUMBERS = [(60, 79), (60, 79), (60, 79), (1, 19), (20, 39), (1, 19), (80, 89), (90, 99), (90, 99), (90, 99),
           (40, 59), (40, 59), (20, 39), (1, 19), (20, 39)]


def rgb(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def font(size):
    return ImageFont.truetype(FONT, size)


def lettering(text, fill, outline, inner=None, height=60, shear=0.22, stroke=None):
    """Bold italic text on a transparent image: an outer outline, optional inner (white) line, then the fill."""
    f = font(height)
    stroke = stroke or max(2, height // 9)
    pad = stroke * 3 + height // 2
    l, t, r, b = ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), text, font=f)
    w, h = r - l + 2 * pad, b - t + 2 * pad
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    xy = (pad - l, pad - t)
    d.text(xy, text, font=f, fill=outline, stroke_width=stroke * 2, stroke_fill=outline)
    if inner:
        d.text(xy, text, font=f, fill=inner, stroke_width=stroke, stroke_fill=inner)
    d.text(xy, text, font=f, fill=fill)
    if shear:
        img = img.transform((w + int(h * shear), h), Image.AFFINE, (1, shear, -h * shear, 0, 1, 0), Image.BICUBIC)
    return img.crop(img.getbbox())


def fit(img, w, h, margin=0):
    """Scale img to fit inside w x h (keeping its shape), centred on a transparent canvas."""
    k = min((w - 2 * margin) / img.width, (h - 2 * margin) / img.height)
    img = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.alpha_composite(img, ((w - img.width) // 2, (h - img.height) // 2))
    return out


def stretch(img, w, h, margin=0):
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.alpha_composite(img.resize((w - 2 * margin, h - 2 * margin), Image.LANCZOS), (margin, margin))
    return out


def shadow(img, offset=(2, 2), blur=1.5, alpha=150):
    a = img.getchannel("A").filter(ImageFilter.GaussianBlur(blur)).point(lambda v: v * alpha // 255)
    sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sh.putalpha(a)
    out = Image.new("RGBA", img.size, (0, 0, 0, 0))
    out.alpha_composite(sh, offset)
    out.alpha_composite(img)
    return out


def badge(t, w, h):
    """Team select logo: the school's initials, outlined, over a swoosh in the second colour."""
    S = 4
    W, H = w * S, h * S
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    p, s = rgb(t["primary"]), rgb(t["secondary"])
    d.ellipse((W * 0.06, H * 0.58, W * 0.94, H * 0.96), fill=s)
    d.ellipse((W * 0.10, H * 0.60, W * 0.98, H * 0.90), fill=(0, 0, 0, 0))
    letters = lettering(t["initials"], p, (255, 255, 255, 255), None, height=int(H * 0.6), stroke=int(H * 0.035))
    letters = lettering(t["initials"], p, rgb("#101018"), (255, 255, 255, 255), height=int(H * 0.6), stroke=int(H * 0.03))
    img.alpha_composite(fit(letters, W, int(H * 0.86), margin=int(H * 0.04)), (0, 0))
    img = img.resize((w, h), Image.LANCZOS)
    return shadow(img, (2, 2), 1.2, 140)


def plate(text, w, h):
    """VS screen city banner: our own plate (dark glass, light bevel, a thin cyan rule) and yellow lettering."""
    S = 4
    W, H = w * S, h * S
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cut = H // 2
    poly = [(cut, 0), (W - cut, 0), (W, cut), (W - cut, H), (cut, H), (0, cut)]
    d.polygon(poly, fill=(170, 176, 190, 255))
    inset = [(cut + 6 * S, 3 * S), (W - cut - 6 * S, 3 * S), (W - 10 * S, cut), (W - cut - 6 * S, H - 3 * S),
             (cut + 6 * S, H - 3 * S), (10 * S, cut)]
    d.polygon(inset, fill=(12, 22, 48, 255))
    for x in range(cut + 10 * S, W - cut - 10 * S, 12 * S):
        d.line((x, 4 * S, x, H - 4 * S), fill=(30, 70, 90, 255), width=S)
    d.line((cut + 6 * S, H - 6 * S, W - cut - 6 * S, H - 6 * S), fill=(60, 200, 220, 255), width=S)
    words = lettering(text.upper(), (255, 236, 40, 255), rgb("#101018"), None, height=int(H * 0.62), shear=0.25, stroke=S)
    img.alpha_composite(fit(words, W - 2 * cut - 20 * S, H - 8 * S), (cut + 10 * S, 4 * S))
    return img.resize((w, h), Image.LANCZOS)


def wordmark(text, fill, outline, w, h, inner=(255, 255, 255, 255)):
    img = lettering(text.upper(), fill, outline, inner, height=h * 4, shear=0.2, stroke=max(2, h // 5))
    return shadow(stretch(img, w, h, margin=1), (1, 1), 0.8, 120)


def helmet(t, w, h, facing="right"):
    """VS screen helmet, facing right: shell in the first colour, a centre stripe in the second, grey facemask,
    the initials on the side."""
    S = 4
    W, H = w * S, h * S
    p, s = rgb(t["primary"]), rgb(t["secondary"])
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    shell = (int(W * 0.08), int(H * 0.04), int(W * 0.72), int(H * 1.18))
    mask = Image.new("L", (W, H), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse(shell, fill=255)
    md.rectangle((0, int(H * 0.86), W, H), fill=0)
    md.polygon([(int(W * 0.60), int(H * 0.55)), (int(W * 0.74), int(H * 0.62)), (int(W * 0.70), int(H * 0.86)),
                (int(W * 0.56), int(H * 0.86))], fill=255)
    body = Image.new("RGBA", (W, H), p)
    # light from the upper left
    glow = Image.new("L", (W, H), 0)
    ImageDraw.Draw(glow).ellipse((int(W * 0.14), int(H * 0.08), int(W * 0.46), int(H * 0.42)), fill=120)
    glow = glow.filter(ImageFilter.GaussianBlur(W // 30))
    body = Image.composite(Image.new("RGBA", (W, H), (255, 255, 255, 255)), body, glow)
    dark = Image.new("L", (W, H), 0)
    ImageDraw.Draw(dark).ellipse((int(W * 0.20), int(H * 0.40), int(W * 0.90), int(H * 1.40)), fill=110)
    dark = dark.filter(ImageFilter.GaussianBlur(W // 25))
    body = Image.composite(Image.new("RGBA", (W, H), (0, 0, 0, 255)), body, dark)
    img.paste(body, (0, 0), mask)
    # stripe over the crown
    stripe = Image.new("L", (W, H), 0)
    sd = ImageDraw.Draw(stripe)
    sd.arc((shell[0] + int(W * 0.02), shell[1], shell[2] - int(W * 0.02), shell[3]), 195, 330, fill=255, width=int(H * 0.11))
    stripe = Image.composite(stripe, Image.new("L", (W, H), 0), mask)
    img.paste(Image.new("RGBA", (W, H), s), (0, 0), stripe)
    # ear hole
    d.ellipse((int(W * 0.40), int(H * 0.56), int(W * 0.48), int(H * 0.70)), fill=(30, 30, 34, 255))
    # facemask
    grey, edge = (190, 192, 198, 255), (70, 72, 80, 255)
    bars = [((0.58, 0.50), (0.93, 0.50)), ((0.60, 0.66), (0.95, 0.68)), ((0.62, 0.80), (0.90, 0.84)),
            ((0.80, 0.42), (0.80, 0.86)), ((0.92, 0.48), (0.92, 0.82))]
    for (x0, y0), (x1, y1) in bars:
        d.line((int(W * x0), int(H * y0), int(W * x1), int(H * y1)), fill=edge, width=int(H * 0.06))
        d.line((int(W * x0), int(H * y0), int(W * x1), int(H * y1)), fill=grey, width=int(H * 0.035))
    letters = lettering(t["initials"], s, rgb("#101018"), (255, 255, 255, 255), height=int(H * 0.30), shear=0.1, stroke=int(H * 0.012))
    x = int(W * 0.18)
    if facing == "left":  # the right-side VS helmet: the shell mirrored, the initials still reading forwards
        img = img.transpose(Image.FLIP_LEFT_RIGHT)
        x = W - x - int(W * 0.32)
    img.alpha_composite(fit(letters, int(W * 0.32), int(H * 0.30)), (x, int(H * 0.30)))
    img = img.resize((w, h), Image.LANCZOS)
    return shadow(img, (3, 3), 2, 120)


def endzone_strips(t, w, h):
    """Both end zone strips (w x h each, w=32 h=120): the nickname along a 2h x w band, as the game lays it out
    (flipped vertically, turned a quarter, first half then second)."""
    band = Image.new("RGBA", (2 * h, w), rgb(t["primary"]))
    words = lettering(t["nickname"].upper(), rgb(t["secondary"]), (255, 255, 255, 255), None, height=96, shear=0.0, stroke=6)
    band.alpha_composite(fit(words, 2 * h, w, margin=3))
    strip = band.transpose(Image.FLIP_TOP_BOTTOM).rotate(-90, expand=True)
    return strip.crop((0, 0, w, h)), strip.crop((0, h, w, 2 * h))


def encode_ci4(img, flags=0, ox=0, oy=0):
    """Type 16: 4-bit colour index, high nibble first, then a 16-entry RGBA5551 palette."""
    img = img.convert("RGB")
    q = img.quantize(colors=16, method=Image.MEDIANCUT)
    pal = q.getpalette()[:48] + [0] * max(0, 48 - len(q.getpalette()[:48]))
    idx = list(q.get_flattened_data() if hasattr(q, "get_flattened_data") else q.getdata())
    w, h = img.size
    px = bytes((idx[i] << 4) | idx[i + 1] for i in range(0, w * h, 2))
    palette = b"".join(struct.pack(">H", wms.to5551(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2], 255)) for i in range(16))
    return b"wn\x02" + bytes([flags]) + struct.pack(">IIIii", 16, w, h, ox, oy) + bytes(8) + px + palette


def team_block(rom, franchise):
    """The ROM offset of a franchise's team block."""
    for k in range(32):
        b = BLOCKS + k * BLOCK
        nick = rom[b + 0x1E0:b + 0x1F0].split(b"\0")[0].decode()
        want = NICKNAMES[franchise]
        if nick in (want if isinstance(want, tuple) else (want,)):
            return b
    raise SystemExit(f"{franchise} isn't in the ROM's team list")


def base_files(rom, ents_by_index, franchise):
    """The base team's file names and headers, by role."""
    b = team_block(rom, franchise)
    u16 = lambda o: struct.unpack(">H", rom[b + o:b + o + 2])[0]
    roles = {"helmet_home": 0x180, "helmet_away": 0x182, "logo_small": 0x1D8, "logo": 0x1DA, "title_city": 0x1DC,
             "title_team": 0x1DE, "endzone1": 0x214, "endzone2": 0x216, "endzone3": 0x218, "win": 0x21A, "vs_left": 0x21C}
    out = {}
    for role, o in roles.items():
        e = ents_by_index[u16(o) - 1]
        d = fs.read(rom, e)
        typ, w, h, ox, oy = struct.unpack_from(">IIIii", d, 4)
        out[role] = dict(name=e["name"].lower(), type=typ, w=w, h=h, origin=(ox, oy), flags=d[3], data=d)
    return out


def helmet_shell(info):
    """The side decal (helmet B) without the NFL mark: the shell colour at its edges, everywhere."""
    if info["type"] != 8:
        return None
    img, _ = wms.decode(info["data"])
    w, h = img.size
    edge = [img.getpixel((x, y)) for x in range(w) for y in (0, h - 1)] + [img.getpixel((x, y)) for y in range(h) for x in (0, w - 1)]
    edge = [p for p in edge if p[3] > 0]
    edge.sort(key=lambda p: p[0] + p[1] + p[2])
    c = edge[len(edge) // 2] if edge else (0, 0, 0, 255)
    return Image.new("RGBA", (w, h), c)


def pack_header(cfg):
    """pack.json without its teams, from the league config (the game reads league name, modes, ladder, ladder_done)."""
    league = {"name": cfg["name"], "modes": cfg.get("modes", ["arcade"])}
    for k in ("ladder", "ladder_done"):
        if k in cfg:
            league[k] = cfg[k]
    return {"format": "blitz-pack/2", "id": cfg["id"], "title": cfg["title"], "version": cfg["version"],
            "requires_rom": "blitz2000-teamplus", "league": league}


def players(t, rng):
    used, out = set(), []
    for slot, (lo, hi) in enumerate(NUMBERS):
        n = rng.randint(lo, hi)
        while n in used:
            n = rng.randint(lo, hi)
        used.add(n)
        out.append({"slot": slot, "last": rng.choice(SURNAMES), "number": n})
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--league", default="pac12", help="tools/leagues/<name>.json, or a path to a league config")
    ap.add_argument("--rom", default=str(ROOT / "build" / "teamplus.z64"))
    ap.add_argument("--out", help="the pak folder (default: the league config's \"out\")")
    ap.add_argument("--sheet", help="the review sheet (default: the league config's \"sheet\")")
    ap.add_argument("--uniforms", help="a folder of finished uniform textures (<ID>/<part>_<home|away>.png at game size, "
                    "build/league_uniforms/README.md): converts them into --out's files/added and points each team at them")
    ap.add_argument("--art", help="a folder of finished art (<ID>/logo.png, logo_small, title_city, title_team, win, helmet, "
                    "endzone at 4x): converts it into --out's files/ instead of drawing. Third-party marks stay out of git: "
                    "point --out at an app folder's packs/pac12 (a user pack wins over the bundled one)")
    ap.add_argument("--handoff", help="a design handoff's league folder (league_pak_art_out/<league>: <ID>/team_art.json, "
                    "uniform_sets.json and their PNGs): the whole pak (art, uniforms with alternates, clashes) into --out")
    ap.add_argument("--research", help="with --handoff: league_pak_research/<league> (<ID>/selected_players.json)")
    ap.add_argument("--report", help="with --handoff: where the import report goes (default build/analysis/league/<id>_import.json)")
    ap.add_argument("--kit", help="with --uniforms: the league's design-kit folder (<ID>/parts.json names each part's game "
                    "file) instead of build/league_uniforms/manifest.json")
    ap.add_argument("--sleeves", help="with --handoff: a list of per-cap shoulder companions (the CFL's component_audit/"
                    "right_arm_assets.json): each set's right sleeve gets its own texture (tools/uv_override.py --sleeves)")
    ap.add_argument("--cart", help="a cart-only texture manifest (JSON, or its folder: {\"sets\": [{team_id, set_id, file, "
                    "sha256}]}): each set's \"cart_files\", what Export ROM puts on the cart instead of the set's own part")
    a = ap.parse_args(argv)
    a.cfg = load_league(a.league)
    if a.cart:
        a.out = a.out or str(ROOT / a.cfg["out"])
        return import_cart(a)
    if a.handoff:
        a.out = a.out or str(ROOT / "build" / "league_paks" / a.cfg["id"])
        import_handoff(a)
        if a.sleeves:
            import uv_override
            uv_override.sleeves(a.sleeves, Path(a.out))
        return
    a.out = a.out or str(ROOT / a.cfg["out"])
    a.sheet = a.sheet or str(ROOT / a.cfg.get("sheet", f"build/analysis/league/{a.cfg['id']}_sheet.png"))
    if a.uniforms:
        return import_uniforms(a)
    if a.art:
        return import_art(a)
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    out = Path(a.out)
    files = out / "files"
    files.mkdir(parents=True, exist_ok=True)
    for old in list(files.glob("*.wms")) + list(files.glob("added/*.wms")):
        old.unlink()
    (files / "added").mkdir(exist_ok=True)
    rng = random.Random(a.cfg.get("player_seed", 2026))
    teams, written, sheet_rows = [], {}, []
    for ct in a.cfg["teams"]:
        tid, base, city, nick, select, abbr = ct["id"], ct["base"], ct["city"], ct["nickname"], ct["select"], ct["abbr"]
        primary, secondary = ct["primary"], ct["secondary"]
        t = dict(id=tid, nickname=nick, initials=ct.get("initials", abbr), primary=primary, secondary=secondary)
        bf = base_files(rom, ents, base)
        art = {
            "logo": badge(t, bf["logo"]["w"], bf["logo"]["h"]),
            "logo_small": badge(t, bf["logo_small"]["w"], bf["logo_small"]["h"]),
            "title_city": plate(city, bf["title_city"]["w"], bf["title_city"]["h"]),
            "title_team": wordmark(nick, rgb(secondary), rgb("#101018"), bf["title_team"]["w"], bf["title_team"]["h"]),
            "win": wordmark(nick, rgb(primary), rgb("#101018"), bf["win"]["w"], bf["win"]["h"]),
            "vs_left": helmet(t, bf["vs_left"]["w"], bf["vs_left"]["h"]),
        }
        ez1, ez2 = endzone_strips(t, bf["endzone1"]["w"], bf["endzone1"]["h"])
        for role, img in art.items():
            info = bf[role]
            data = wms.encode_type19(img, info["flags"] or 4, *info["origin"])
            written.setdefault(info["name"], (tid, data))
        for role, img in (("endzone1", ez1), ("endzone2", ez2),
                          ("endzone3", Image.new("RGBA", (bf["endzone3"]["w"], bf["endzone3"]["h"]), rgb(primary)))):
            info = bf[role]
            if info["type"] == 16 and img.size == (info["w"], info["h"]):
                written.setdefault(info["name"], (tid, encode_ci4(img, info["flags"], *info["origin"])))
        for role in ("helmet_home", "helmet_away"):
            shell = helmet_shell(bf[role])
            if shell is not None:
                written.setdefault(bf[role]["name"], (tid, wms.encode_rgba16(shell, bf[role]["flags"], *bf[role]["origin"])))
        right = helmet(t, bf["vs_left"]["w"], bf["vs_left"]["h"], facing="left")
        info = bf["vs_left"]
        (files / "added" / f"{tid.lower()}_vsr.wms").write_bytes(wms.encode_type19(right, info["flags"] or 4, *info["origin"]))
        sheet_rows.append([art["logo"], art["title_city"], art["title_team"], art["win"], art["vs_left"], right,
                           ez1.rotate(90, expand=True)])
        teams.append({
            "id": tid, "base": base, "city": city, "nickname": nick, "select": select, "abbr": abbr,
            "vs_helmet_right": f"{tid.lower()}_vsr.wms",
            "ratings": team_ratings(ct),
            "players": players(t, rng),
        })
    for name, (tid, data) in sorted(written.items()):
        (files / name).write_bytes(data)
    pack = pack_header(a.cfg)
    pack["teams"] = teams
    (out / "pack.json").write_text(json.dumps(pack, indent=1) + "\n")
    print(f"{out / 'pack.json'}: {len(teams)} teams; {len(written)} images in {files}")

    # A review sheet (build/, not committed).
    rows = []
    for r in sheet_rows:
        hgt = max(i.height for i in r) + 8
        row = Image.new("RGBA", (sum(i.width + 8 for i in r) + 8, hgt), (60, 64, 80, 255))
        x = 8
        for i in r:
            row.alpha_composite(i, (x, (hgt - i.height) // 2))
            x += i.width + 8
        rows.append(row)
    sheet = Image.new("RGBA", (max(r.width for r in rows), sum(r.height for r in rows)), (60, 64, 80, 255))
    y = 0
    for r in rows:
        sheet.alpha_composite(r, (0, y))
        y += r.height
    Path(a.sheet).parent.mkdir(parents=True, exist_ok=True)
    sheet.save(a.sheet)
    print(f"review sheet: {a.sheet}")


UNIFORM_PARTS = [("helmet_side", "hlb"), ("helmet_stripe_front", "hlc"), ("helmet_stripe_back", "hld"), ("facemask", "msk"),
                 ("shoulder", "shl"), ("jersey", "jer"), ("pant_stripe", "pst"), ("pants", "pnt"), ("socks", "sck"), ("numbers", "num")]


def encode_ci4_exact(img, flags=0, ox=0, oy=0):
    """Type 16 from an image of at most 16 colours, transparency kept (a palette entry with its alpha bit clear)."""
    img = img.convert("RGBA")
    px = list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())
    px = [(0, 0, 0, 0) if p[3] < 128 else (p[0], p[1], p[2], 255) for p in px]
    colours = list(dict.fromkeys(px))
    if len(colours) > 16:
        raise SystemExit(f"{len(colours)} colours; a 16-colour part allows 16")
    colours += [(0, 0, 0, 0)] * (16 - len(colours))
    index = {c: i for i, c in enumerate(colours)}
    w, h = img.size
    body = bytes((index[px[i]] << 4) | index[px[i + 1]] for i in range(0, w * h, 2))
    palette = b"".join(struct.pack(">H", wms.to5551(*c)) for c in colours)
    return b"wn\x02" + bytes([flags]) + struct.pack(">IIIii", 16, w, h, ox, oy) + bytes(8) + body + palette


def import_cart(a):
    """Cart-only textures (a design handoff's manifest) -> files/added/<the set's own file>c.wms and the set's "cart_files".

    The manifest (a JSON file, or a folder holding one, e.g. cart_safe_shoulders/cart_safe_shoulders.json) lists
    {"sets": [{"team_id", "set_id", "file", "sha256"?, "part"?}]}: "file" is a PNG relative to the manifest, at the set's
    own part's size, in the game's stored layout; "part" (or the manifest's, or the file name's "<part>_cart.png") names
    the uniform part. Each goes in at the set's own file's size, format, flags and origin. Export ROM puts it on the cart
    in place of the set's own file (src/game/content_pack_tp.cpp league_cart_part), for sets whose look the cart's code
    can't draw (a "uv_override"'s coordinates); the app never draws it. Run it last (after --uniforms and
    tools/uv_override.py): pack.json's teams must already have their sets."""
    m = Path(a.cart)
    if m.is_dir():
        found = [p for p in sorted(m.glob("*.json")) if isinstance(json.loads(p.read_text()).get("sets"), list)]
        if len(found) != 1:
            raise SystemExit(f"{m}: {len(found)} manifests with \"sets\" (want one)")
        m = found[0]
    manifest = json.loads(m.read_text())
    out = Path(a.out)
    pack_path = out / "pack.json"
    pack = json.loads(pack_path.read_text())
    if manifest.get("league") and manifest["league"] != pack.get("id"):
        raise SystemExit(f"{m}: for league {manifest['league']}, the pak is {pack.get('id')}")
    added = out / "files" / "added"
    parts = [p for p, _ in UNIFORM_PARTS]
    done = []
    for e in manifest["sets"]:
        tid, sid = e["team_id"], e["set_id"]
        png = m.parent / e["file"]
        data = png.read_bytes()
        if e.get("sha256") and hashlib.sha256(data).hexdigest() != e["sha256"]:
            raise SystemExit(f"{png}: SHA-256 {hashlib.sha256(data).hexdigest()}, the manifest says {e['sha256']}")
        part = e.get("part") or manifest.get("part") or png.stem.removesuffix("_cart")
        if part not in parts:
            raise SystemExit(f"{png}: part {part!r} isn't one of {parts}")
        team = next((t for t in pack["teams"] if t["id"] == tid), None)
        if team is None:
            raise SystemExit(f"{tid}: not a team of {pack_path}")
        if sid in ("home", "away"):
            own = team.get("uniform_files", {}).get(f"{part}_{sid}")
            holder = team.setdefault("cart_files", {}).setdefault(sid, {})
        else:
            us = next((u for u in team.get("uniform_sets", []) if u["id"] == sid), None)
            if us is None:
                raise SystemExit(f"{tid}: no uniform set {sid} in {pack_path}")
            own = us.get("files", {}).get(part)
            holder = us.setdefault("cart_files", {})
        if not own:
            raise SystemExit(f"{tid} {sid}: the set has no {part} file of its own to swap on the cart")
        d = (added / own).read_bytes()
        typ, w, h, ox, oy = struct.unpack_from(">IIIii", d, 4)
        img = Image.open(io.BytesIO(data)).convert("RGBA")
        if img.size != (w, h):
            raise SystemExit(f"{png}: {img.size}, the set's {part} ({own}) is {(w, h)}")
        name = own[:-4] + "c.wms"
        if len(name) > 14:
            raise SystemExit(f"{own}: its cart file {name} is longer than the game's 14 characters")
        enc = encode_ci4_exact(img, d[3], ox, oy) if typ == 16 else wms.encode_rgba16(img, d[3], ox, oy)
        (added / name).write_bytes(enc)
        holder[part] = name
        done.append(f"{tid} {sid}: {part} {own} -> {name} on the cart")
    pack_path.write_text(json.dumps(pack, indent=1) + "\n")
    print("\n".join(done))
    print(f"{pack_path}: \"cart_files\" for {len(done)} set part(s) from {m}")


def import_uniforms(a):
    """Finished uniform PNGs -> files/added/<id>_<part>_<h|a>.wms and each team's "uniform_files"; clashes from jersey colours.

    A team folder with a uniform_sets.json (the 2026 league handoff) also gives: its alternates (alternates/<set_id>/
    <part>.png, listed under "alternates_delivered") -> files/added/<id>_<code>_<n>.wms and the team's "uniform_sets"
    (the Uniform Picker offers them after home and away); its clash suggestions -> "clashes_with"; and its helmet sides
    are stored as drawn (helmet_logo set: halves composed deliberately, e.g. Boise's number half and horse half)."""
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    by_name = fs.by_name(ents)
    src, out = Path(a.uniforms), Path(a.out)
    if getattr(a, "kit", None):
        # The design kit's parts.json: base_game_file per part (same_file_as as the old manifest has it).
        manifest = {"teams": {}}
        for pj in sorted(Path(a.kit).glob("*/parts.json")):
            parts = json.loads(pj.read_text())
            manifest["teams"][pj.parent.name] = {"parts": {k: dict(v, game_file=v["base_game_file"]) for k, v in parts.items()}}
    else:
        manifest = json.loads((ROOT / a.cfg["uniform_manifest"]).read_text())
    profile_logos, backwards = set(a.cfg.get("profile_logos", [])), a.cfg.get("helmet_backwards", "none")
    pack = json.loads((out / "pack.json").read_text())
    added = out / "files" / "added"
    added.mkdir(parents=True, exist_ok=True)
    colour, n, suggested = {}, 0, {}

    def encode_part(tid, part, meta, png, as_drawn):
        d = fs.read(rom, by_name[meta["game_file"]])
        typ, w, h, ox, oy = struct.unpack_from(">IIIii", d, 4)
        img = Image.open(png).convert("RGBA")
        if img.size != (w, h):
            raise SystemExit(f"{png}: {img.size}, the game's is {(w, h)}")
        if part == "helmet_side":
            # Painted as on the helmet; uniform_handoff.helmet_store turns it into the game's layout
            # (rows 14-29 flipped, the halves mirror images of each other).
            from uniform_handoff import helmet_store
            # Older Pac-12 art drew its logos facing right in both halves, so a profile logo's left half was backwards.
            # Art with helmet_logo metadata has its halves composed as they should look: stored as drawn.
            img = helmet_store(img, backwards, profile=not as_drawn and tid in profile_logos)
        # Numbers and every other part: stored as delivered (the kit's templates are the game's stored layout, digits
        # included, and the handoff keeps their alpha masks pixel for pixel).
        return img, (encode_ci4_exact(img, d[3], ox, oy) if typ == 16 else wms.encode_rgba16(img, d[3], ox, oy))

    for team in pack["teams"]:
        tid = team["id"]
        parts = manifest["teams"].get(tid, {}).get("parts", {})
        sets_meta = json.loads((src / tid / "uniform_sets.json").read_text()) if (src / tid / "uniform_sets.json").exists() else None
        as_drawn = bool(sets_meta) and all(sets_meta.get(s, {}).get("helmet_logo") for s in ("home", "away"))
        files = {}
        for part, code in UNIFORM_PARTS:
            for side, s in (("home", "h"), ("away", "a")):
                key = f"{part}_{side}"
                meta = parts.get(key)
                if not meta:
                    continue
                png = src / tid / f"{key}.png"
                if not png.exists():
                    # The base shares one file for both sides: the away side wears what the home one became,
                    # unless the art gives it its own (added files are per side).
                    if meta.get("same_file_as") in files:
                        files[key] = files[meta["same_file_as"]]
                    continue
                img, data = encode_part(tid, part, meta, png, as_drawn)
                name = f"{tid.lower()}_{code}_{s}.wms"
                (added / name).write_bytes(data)
                files[key] = name
                n += 1
                if key == "jersey_home":
                    opaque = [p for p in (img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()) if p[3] >= 128]
                    colour[tid] = tuple(sum(p[i] for p in opaque) / len(opaque) for i in range(3))
        if files:
            team["uniform_files"] = files
        # Alternates: alternates/<set_id>/<part>.png, the parts' game files those of the side the set was drawn on.
        alts = []
        for k, alt in enumerate((sets_meta or {}).get("alternates_delivered", []), 1):
            folder = src / tid / alt.get("directory", f"alternates/{alt['set_id']}")
            side = alt.get("template_basis", "home")
            set_files = {}
            for part, code in UNIFORM_PARTS:
                png, meta = folder / f"{part}.png", parts.get(f"{part}_{side}")
                if not png.exists() or not meta:
                    continue
                _, data = encode_part(tid, part, meta, png, bool(alt.get("helmet_logo")))
                name = f"{tid.lower()}_{code}_{k}.wms"
                (added / name).write_bytes(data)
                set_files[part] = name
                n += 1
            if set_files:
                alts.append({"id": alt["set_id"], "name": alt["name"][:31], "dark": bool(alt.get("dark", True)), "files": set_files})
        if alts:
            team["uniform_sets"] = alts
        else:
            team.pop("uniform_sets", None)
        if sets_meta and "clash_suggestions" in sets_meta:
            # "opponent_id" (the first Pac-12 handoff) or "with" (the 2026 per-set format import_handoff reads)
            suggested[tid] = [c.get("opponent_id") or c.get("with") for c in sets_meta["clash_suggestions"]]
    ids = [t["id"] for t in pack["teams"]]
    for team in pack["teams"]:
        tid = team["id"]
        if suggested:
            # The handoff's clash suggestions (each pair: one side in its road set), made symmetric.
            near = [o for o in ids if o != tid and (o in suggested.get(tid, []) or tid in suggested.get(o, []))]
        elif tid in colour:
            # Home jerseys that look alike clash (the second team switches to its away set), as the game's matrix does.
            near = [o for o in colour if o != tid and sum((x - y) ** 2 for x, y in zip(colour[tid], colour[o])) ** 0.5 < 70]
        else:
            continue
        if near:
            team["clashes_with"] = near
        else:
            team.pop("clashes_with", None)
    (out / "pack.json").write_text(json.dumps(pack, indent=1) + "\n")
    print(f"{out}: {n} uniform textures from {src}; alternates: " +
          ", ".join(f"{t['id']} {'/'.join(s['id'] for s in t['uniform_sets'])}" for t in pack["teams"] if t.get("uniform_sets")) +
          "; clashes: " + ", ".join(f"{t['id']}~{'/'.join(t['clashes_with'])}" for t in pack["teams"] if t.get("clashes_with")))


# A league config's "profile_logos": teams whose helmet logo is an animal in profile. --art (art with no right-side
# VS helmet of its own) makes theirs the left one mirrored whole, so the animal faces the facemask on both sides.
# Letter marks (and a front-facing head) read the same on both sides.
TITLE_TEAM_MAX_H = 32  # px: the nickname wordmark on the VS screen; taller boxes (CIN's is 46) push it off the banner


def fit_wordmark(img, w, h):
    """The wordmark in its base team's box, no taller than TITLE_TEAM_MAX_H, keeping its proportions, bottom-centred."""
    if h <= TITLE_TEAM_MAX_H:
        return img.resize((w, h), Image.LANCZOS)
    scale = min(w / img.width, TITLE_TEAM_MAX_H / img.height)
    small = img.resize((max(1, round(img.width * scale)), max(1, round(img.height * scale))), Image.LANCZOS)
    box = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    box.alpha_composite(small, ((w - small.width) // 2, h - small.height))
    return box


def endzone_band_strips(band, w, h):
    """End zone strips (w x h each, the game's ez1 and ez2) from a band 2h wide and w high read left to right.
    The game stores its strips mirrored (tools/import_team_art.py, commit 7dddc82: the NFL rounds' readable strips,
    the band turned clockwise, are mirrored left to right). Flipping the band top to bottom and turning it clockwise
    is that same mirror: pixel (x, y) of the band is (y, x) of the strip pair, ez1 the band's first half.
    In-game proof for a league pak: Boise State, 2026-11-27 (pac12_dev_proof/BSU_ENDZONE_PROOF.md)."""
    strip = band.transpose(Image.FLIP_TOP_BOTTOM).rotate(-90, expand=True)
    return strip.crop((0, 0, w, h)), strip.crop((0, h, w, 2 * h))


def import_art(a):
    """Finished 4x PNGs -> the pak's .wms files, sized and placed like the drawn set; pack.json copied from the league's own (config "out").

    A team with a team_art.json (the 2026 league handoff) also uses its endzone_fill colour; a team without endzone.png
    keeps its base team's end zone (bands are made only once the orientation is proven in game)."""
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    art, out = Path(a.art), Path(a.out)
    files = out / "files"
    files.mkdir(parents=True, exist_ok=True)
    # The repo's pack.json, unless this folder has one already (it may carry real rosters from college_roster.py):
    # then only the art changes and its players stay.
    if not (out / "pack.json").exists():
        (out / "pack.json").write_text((ROOT / a.cfg["out"] / "pack.json").read_text())
    profile_logos = set(a.cfg.get("profile_logos", []))
    n = 0
    for ct in a.cfg["teams"]:
        tid, base = ct["id"], ct["base"]
        if not (art / tid).is_dir():
            continue
        meta = json.loads((art / tid / "team_art.json").read_text()) if (art / tid / "team_art.json").exists() else {}
        primary = meta.get("endzone_fill") or ct["primary"]
        bf = base_files(rom, ents, base)
        load = lambda name, info: Image.open(art / tid / name).convert("RGBA").resize((info["w"], info["h"]), Image.LANCZOS)
        for role, name in (("logo", "logo.png"), ("logo_small", "logo_small.png"), ("title_city", "title_city.png"),
                           ("title_team", "title_team.png"), ("win", "win.png"), ("vs_left", "helmet.png")):
            info = bf[role]
            if role == "logo_small":
                # The scoreboard logo: hard alpha in a palette, as the game's own (type 0). Its master is the native image
                # scaled up by a whole number (nearest neighbour), so nearest neighbour gives the native pixels back.
                img = Image.open(art / tid / name).convert("RGBA").resize((info["w"], info["h"]), Image.NEAREST)
                img.putalpha(img.getchannel("A").point(lambda v: 255 if v >= 128 else 0))
                try:
                    data = wms.encode_ci8(img, info["flags"] or 4, *info["origin"])
                except Exception:  # noqa: BLE001  over 255 colours: hard-alpha full colour
                    data = wms.encode_rgba16(img, info["flags"] or 4, *info["origin"])
                (files / info["name"]).write_bytes(data)
                n += 1
                continue
            img = load(name, info) if role != "title_team" else fit_wordmark(Image.open(art / tid / name).convert("RGBA"), info["w"], info["h"])
            (files / info["name"]).write_bytes(wms.encode_type19(img, info["flags"] or 4, *info["origin"]))
            n += 1
        h, w = bf["endzone1"]["h"], bf["endzone1"]["w"]
        if (art / tid / "endzone.png").exists():
            band = Image.open(art / tid / "endzone.png").convert("RGBA")
            if band.size != (2 * h, w):
                band = band.resize((2 * h, w), Image.LANCZOS)
            ez1, ez2 = endzone_band_strips(band, w, h)
            fill = Image.new("RGBA", (bf["endzone3"]["w"], bf["endzone3"]["h"]), rgb(primary))
            for role, img in (("endzone1", ez1), ("endzone2", ez2), ("endzone3", fill)):
                info = bf[role]
                # Flat art in at most 16 colours keeps its exact palette (both strips and the fill then match); else quantised.
                exact = len(img.convert("RGB").getcolors(16) or []) > 0
                (files / info["name"]).write_bytes(encode_ci4_exact(img, info["flags"], *info["origin"]) if exact
                                                   else encode_ci4(img, info["flags"], *info["origin"]))
                n += 1
        else:
            for role in ("endzone1", "endzone2", "endzone3"):
                (files / bf[role]["name"]).unlink(missing_ok=True)
            print(f"{tid}: no endzone.png; its base's end zone stays")
        for role in ("helmet_home", "helmet_away"):
            shell = helmet_shell(bf[role])
            if shell is not None:
                (files / bf[role]["name"]).write_bytes(wms.encode_rgba16(shell, bf[role]["flags"], *bf[role]["origin"]))
        right = art / tid / "helmet_right.png"
        added = files / "added" / f"{tid.lower()}_vsr.wms"
        if right.exists() or tid in profile_logos:
            info = bf["vs_left"]
            added.parent.mkdir(exist_ok=True)
            if right.exists():
                # The art's own right-side helmet (facing left), never the left one mirrored: lettering and numbers
                # would read backwards, and a helmet's two sides can differ (Boise: horse on one, 26 on the other).
                img = Image.open(right).convert("RGBA").resize((info["w"], info["h"]), Image.LANCZOS)
            else:
                img = load("helmet.png", info).transpose(Image.FLIP_LEFT_RIGHT)  # older art without one: a profile logo
            added.write_bytes(wms.encode_type19(img, info["flags"] or 4, *info["origin"]))
            n += 1
        elif added.exists():
            added.unlink()  # the drawn one wouldn't match this art; the left helmet mirrored is closer
    # Point each team at its right helmet when the art has one (an older local pack.json may lack the key).
    pack = json.loads((out / "pack.json").read_text())
    for team in pack["teams"]:
        name = f"{team['id'].lower()}_vsr.wms"
        if (files / "added" / name).exists():
            team["vs_helmet_right"] = name
        else:
            team.pop("vs_helmet_right", None)
    (out / "pack.json").write_text(json.dumps(pack, indent=1) + "\n")
    print(f"{out}: {n} images from {art}")


# ---- A design handoff (league_pak_art_out/<league> + league_pak_research/<league>) -> a complete pak ----------------
# Per team: team_art.json (2D roles, each file's native size and scale), uniform_sets.json (home, away, alternates with
# their parts, dark flag, helmet_logo and symmetric clash suggestions), the research's selected_players.json (each
# player's "blitz" object holds only pack.json's player keys). Any league in that layout (CFL, Pac-12 2026).

PLAYER_KEYS = {"slot", "expect", "last", "number", "size", "skin", "heavy", "announcer_call"}
ROLE_OF = {"logo": "logo", "logo_small": "logo_small", "title_city": "title_city", "title_team": "title_team", "win": "win",
           "helmet": "vs_left", "helmet_right": "vs_left"}


def base_uniform_parts(rom, ents_by_index, franchise):
    """The base team's ten uniform parts per side: [part][side] -> name, type, size, origin, flags."""
    b = team_block(rom, franchise)
    out = []
    for k in range(10):
        row = []
        for side in (0, 1):
            u = struct.unpack(">H", rom[b + 0x180 + 4 * k + 2 * side:b + 0x182 + 4 * k + 2 * side])[0]
            e = ents_by_index[u - 1]
            d = fs.read(rom, e)
            typ, w, h, ox, oy = struct.unpack_from(">IIIii", d, 4)
            row.append(dict(name=e["name"].lower(), type=typ, w=w, h=h, origin=(ox, oy), flags=d[3]))
        out.append(row)
    return out


def soft_alpha(img):
    px = img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()
    return any(0 < p[3] < 255 for p in px)


def endzone_band_to_strips(band, w, h):
    """A readable end-zone band (2h x w, e.g. 240x32, the word left to right) -> the two stored strips (w x h each).

    The game's own strips are stored like this: the Bills' bil_ez1 on top of bil_ez2 read as the band after a quarter
    turn anticlockwise and a vertical flip, so the band goes back by the inverse (flip, quarter turn clockwise), the
    first strip the band's left half. Same transform as endzone_strips (draw mode) and --art."""
    strip = band.transpose(Image.FLIP_TOP_BOTTOM).rotate(-90, expand=True)
    return strip.crop((0, 0, w, h)), strip.crop((0, h, w, 2 * h))


def import_handoff(a):
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    src, out, cfg = Path(a.handoff), Path(a.out), a.cfg
    research = Path(a.research) if a.research else None
    files = out / "files"
    added = files / "added"
    files.mkdir(parents=True, exist_ok=True)
    for old in list(files.glob("*.wms")) + list(files.glob("added/*.wms")):
        old.unlink()
    added.mkdir(exist_ok=True)
    report = {"league": cfg["id"], "handoff": str(src), "research": str(research) if research else None, "teams": {},
              "rejected": [], "warnings": [], "written": 0}
    reject = lambda tid, s, role, f, why, **kw: report["rejected"].append(dict(team=tid, set=s, role=role, file=str(f), why=why, **kw))
    warn = lambda msg: report["warnings"].append(msg)
    backwards = cfg.get("helmet_backwards", "none")
    ids = [t["id"] for t in cfg["teams"]]
    teams, home_rgb = [], {}
    set_ids = {}  # team -> its set ids (home, away, alternates), for the clash lists

    def write(path, data):
        path.write_bytes(data)
        report["written"] += 1

    for ct in cfg["teams"]:
        tid, base = ct["id"], ct["base"]
        tdir = src / tid
        tr = report["teams"].setdefault(tid, {"base": base, "art": {}, "uniforms": {}, "players": 0})
        ta = json.loads((tdir / "team_art.json").read_text())
        if ta.get("base") and ta["base"] != base:
            raise SystemExit(f"{tid}: the art was made on {ta['base']}, the league config says {base}")
        bf = base_files(rom, ents, base)
        team = {"id": tid, "base": base, "city": ct["city"], "nickname": ct["nickname"], "select": ct["select"], "abbr": ct["abbr"]}

        # 2D roles: each file at its own scale (team_art.json file_specs), to the base team's slot.
        specs = ta.get("file_specs", {})
        for role, rel in ta.get("files", {}).items():
            spec = specs.get(role, {})
            path = src / rel
            if role == "endzone":
                continue
            if role not in ROLE_OF:
                warn(f"{tid}: role {role} isn't used")
                continue
            info = bf[ROLE_OF[role]]
            if not path.exists():
                reject(tid, "2d", role, path, "missing")
                continue
            img = Image.open(path).convert("RGBA")
            scale = spec.get("scale", 4)
            native = tuple(spec.get("native_dimensions", (info["w"], info["h"])))
            want = (native[0] * scale, native[1] * scale)
            if native != (info["w"], info["h"]):
                reject(tid, "2d", role, path, f"native size {native} isn't the {base} slot {(info['w'], info['h'])}",
                       expected=[info["w"] * scale, info["h"] * scale], actual=list(img.size))
                continue
            if img.size != want:
                reject(tid, "2d", role, path, f"{img.size} isn't {native} x{scale} = {want}", expected=list(want), actual=list(img.size))
                continue
            if scale != 1:
                img = img.resize(native, Image.LANCZOS)
            note = {}
            if role == "title_team":
                box = img.getchannel("A").point(lambda v: 255 if v >= 16 else 0).getbbox()
                note["visible_height"] = box[3] - box[1] if box else 0
                note["bottom_gap"] = native[1] - box[3] if box else None
                if box and box[3] - box[1] > TITLE_TEAM_MAX_H:
                    img = fit_wordmark(img.crop(box), info["w"], info["h"])
                    warn(f"{tid}: title_team {box[3] - box[1]} px tall, capped to {TITLE_TEAM_MAX_H}")
            soft = soft_alpha(img)
            data = (wms.encode_type19 if soft or role != "logo_small" else wms.encode_rgba16)(img, info["flags"] or 4, *info["origin"])
            if role == "helmet_right":
                write(added / f"{tid.lower()}_vsr.wms", data)
                team["vs_helmet_right"] = f"{tid.lower()}_vsr.wms"
                tr["art"][role] = f"added/{tid.lower()}_vsr.wms"
            else:
                write(files / info["name"], data)
                tr["art"][role] = info["name"]
            tr["art"][role + "_alpha"] = "soft" if soft else "hard"
            tr["art"].update({f"{role}_{k}": v for k, v in note.items()})
        if "vs_helmet_right" not in team:
            warn(f"{tid}: no helmet_right.png; the right VS helmet is the left one mirrored by the game")

        # End zone: a native band (2h x w), or a plain placeholder in the fill colour (no lettering, no NFL strip).
        ez_w, ez_h = bf["endzone1"]["w"], bf["endzone1"]["h"]
        fill = ta.get("endzone_fill") or ta.get("colours", {}).get("primary") or ct.get("primary", "#404040")
        rel = ta.get("files", {}).get("endzone")
        band = None
        if rel and (src / rel).exists():
            band = Image.open(src / rel).convert("RGBA")
            if band.size != (2 * ez_h, ez_w):
                reject(tid, "2d", "endzone", src / rel, f"{band.size} isn't the native {(2 * ez_h, ez_w)} band",
                       expected=[2 * ez_h, ez_w], actual=list(band.size))
                band = None
        if band is None:
            band = Image.new("RGBA", (2 * ez_h, ez_w), rgb(fill))
            tr["art"]["endzone"] = f"placeholder: plain {fill}, no band delivered"
        else:
            tr["art"]["endzone"] = f"band {rel}, fill {fill}"
        ez1, ez2 = endzone_band_to_strips(band, ez_w, ez_h)
        for role, img in (("endzone1", ez1), ("endzone2", ez2), ("endzone3", Image.new("RGBA", (bf["endzone3"]["w"], bf["endzone3"]["h"]), rgb(fill)))):
            info = bf[role]
            write(files / info["name"], encode_ci4_exact(img, info["flags"], *info["origin"]))
        for role in ("helmet_home", "helmet_away"):  # the base's NFL helmet decal, should anything still draw it
            shell = helmet_shell(bf[role])
            if shell is not None:
                write(files / bf[role]["name"], wms.encode_rgba16(shell, bf[role]["flags"], *bf[role]["origin"]))

        # Uniforms: home, away and the alternates, each part at the base part's size and format.
        parts = base_uniform_parts(rom, ents, base)
        us = json.loads((tdir / "uniform_sets.json").read_text())
        sets = [("home", us["home"], 0), ("away", us["away"], 1)]
        for k, alt in enumerate(us.get("alternates_delivered", [])):
            side = 1 if alt.get("template_side", "home") == "away" else 0
            sj = tdir / alt.get("directory", "") / "set.json"
            if sj.exists() and not alt.get("template_side"):
                side = 1 if json.loads(sj.read_text()).get("template_side") == "away" else 0
            sets.append((alt["set_id"], alt, side))
        set_ids[tid] = [s for s, _, _ in sets]
        uniform_files, extra = {}, []
        for n, (sid, meta, side) in enumerate(sets):
            code_side = "h" if sid == "home" else "a" if sid == "away" else str(n)
            got = {}
            for k, (part, code) in enumerate(UNIFORM_PARTS):
                rel = meta.get("files", {}).get(part)
                path = tdir / rel if rel else None
                if not path or not path.exists():
                    reject(tid, sid, part, path, "missing")
                    continue
                info = parts[k][side]
                img = Image.open(path).convert("RGBA")
                if img.size != (info["w"], info["h"]):
                    reject(tid, sid, part, path, f"{img.size} isn't the {base} part's {(info['w'], info['h'])}",
                           expected=[info["w"], info["h"]], actual=list(img.size))
                    continue
                if part == "helmet_side":
                    from uniform_handoff import helmet_store, _halves_alike
                    same, mirrored = _halves_alike(img)
                    logo = meta.get("helmet_logo", "symmetric")
                    # Painted as on the helmet; only the game's layout change (rows 14-29 flipped, right half mirrored).
                    # A profile logo already drawn as mirror-image halves is left as drawn: no global mirror of a decal.
                    img = helmet_store(img, backwards, profile=logo == "profile")
                    tr["uniforms"][f"{sid}.helmet_logo"] = f"{logo}; halves same {same:.2f} mirrored {mirrored:.2f}"
                try:
                    data = encode_ci4_exact(img, info["flags"], *info["origin"]) if info["type"] == 16 else wms.encode_rgba16(img, info["flags"], *info["origin"])
                except SystemExit as e:
                    reject(tid, sid, part, path, str(e), expected="16 colours incl. transparency")
                    continue
                name = f"{tid.lower()}_{code}_{code_side}.wms"
                write(added / name, data)
                got[part] = name
                if sid == "home" and part == "jersey":
                    opaque = [p for p in (img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()) if p[3] >= 128]
                    home_rgb[tid] = tuple(sum(p[i] for p in opaque) / len(opaque) for i in range(3))
            if sid in ("home", "away"):
                uniform_files.update({f"{p}_{sid}": f for p, f in got.items()})
            elif got:
                extra.append({"id": sid, "name": meta.get("name", sid)[:31], "dark": bool(meta.get("dark", True)), "files": got})
            tr["uniforms"][sid] = f"{len(got)}/10 parts"
        team["uniform_files"] = uniform_files
        # Set metadata for the picker and the clash check (src/game/content_pack_tp.cpp "uniform_sets").
        meta_sets = [{"id": "home", "name": us["home"].get("name", "Home")[:31], "dark": bool(us["home"].get("dark", True))},
                     {"id": "away", "name": us["away"].get("name", "Away")[:31], "dark": bool(us["away"].get("dark", False))}]
        team["uniform_sets"] = meta_sets + extra
        team["_clash_suggestions"] = us.get("clash_suggestions", [])

        # Players: the research's selection, only the parser's keys.
        if research:
            sel = json.loads((research / tid / "selected_players.json").read_text())
            plist = []
            for p in sel["players"]:
                bz = p["blitz"]
                bad = set(bz) - PLAYER_KEYS
                if bad:
                    raise SystemExit(f"{tid}: player keys {sorted(bad)} aren't pack.json's")
                plist.append({k: bz[k] for k in ("slot", "last", "number", "size", "skin", "heavy", "announcer_call") if k in bz})
            plist.sort(key=lambda p: p["slot"])
            slots = [p["slot"] for p in plist]
            if slots != list(range(15)):
                raise SystemExit(f"{tid}: slots {slots}, not 0-14 once each")
            pos = {p["blitz"]["slot"]: p.get("pos_native", "") for p in sel["players"]}
            if pos.get(3) != "QB":
                raise SystemExit(f"{tid}: slot 3 is {pos.get(3)}, not the QB")
            c1 = next(p for p in sel["players"] if p["blitz"]["slot"] == 1)
            if pos.get(1) not in ("C", "OC") and not any(w in (c1.get("selection_rationale", "") + c1.get("pos_detail", "")).lower()
                                                     for w in ("centre", "center")):
                warn(f"{tid}: slot 1 ({c1['blitz']['last']}, {pos.get(1)}) isn't marked as the centre")
            for p in plist:
                if not (1 <= len(p["last"]) <= 15) or any(not (c.isalpha() or c in " .'-") or ord(c) > 127 for c in p["last"]):
                    raise SystemExit(f"{tid} slot {p['slot']}: label {p['last']!r} isn't 1-15 ASCII letters, space . ' -")
            nums = [p["number"] for p in plist]
            if len(set(nums)) != len(nums):
                warn(f"{tid}: duplicate jersey numbers {nums}")
            team["players"] = plist
            tr["players"] = len(plist)
            tr["qb"] = f"{plist[3]['number']}-{plist[3]['last']}"
            bars = sel.get("team_bars")
            if bars:
                team["ratings"] = {k: bars[k] for k in RATING_NAMES if k in bars}
        if "ratings" not in team and ct.get("ratings"):
            team["ratings"] = team_ratings(ct)
        teams.append(team)

    # Clashes: the suggestions are per set and symmetric; home-on-home ones become the team clash list the game's own
    # rule reads (who changes is the game's choice, not a visitor-wears-away rule), every one goes on its set too.
    pairs = set()
    for team in teams:
        for c in team.pop("_clash_suggestions"):
            if c["with"] not in ids:
                warn(f"{team['id']}: clash with unknown team {c['with']}")
                continue
            if c["with_set"] not in set_ids.get(c["with"], []) or c["set_id"] not in set_ids[team["id"]]:
                warn(f"{team['id']}.{c['set_id']} clash with unknown set {c['with']}.{c['with_set']}")
                continue
            pairs.add(((team["id"], c["set_id"]), (c["with"], c["with_set"])))
    one_way = [p for p in pairs if (p[1], p[0]) not in pairs]
    if one_way:
        warn(f"{len(one_way)} clash suggestion(s) listed one way only, e.g. {one_way[0]}: used both ways")
    for (x, y) in list(pairs):
        pairs.add((y, x))
    for team in teams:
        tid = team["id"]
        homes = sorted({y[0] for x, y in pairs if x == (tid, "home") and y[1] == "home"}, key=ids.index)
        if homes:
            team["clashes_with"] = homes
        for s in team["uniform_sets"]:
            cl = sorted({f"{y[0]}.{y[1]}" for x, y in pairs if x == (tid, s["id"])})
            if cl:
                s["clashes"] = cl
    report["clash_pairs"] = len(pairs) // 2
    pack = pack_header(cfg)
    pack["teams"] = teams
    (out / "pack.json").write_text(json.dumps(pack, indent=1) + "\n")
    rp = Path(a.report or ROOT / "build" / "analysis" / "league" / f"{cfg['id']}_import.json")
    rp.parent.mkdir(parents=True, exist_ok=True)
    rp.write_text(json.dumps(report, indent=1) + "\n")
    print(f"{out}: {len(teams)} teams, {report['written']} files; {len(report['rejected'])} rejected, "
          f"{len(report['warnings'])} warning(s); report {rp}")
    for r in report["rejected"]:
        print(f"  REJECTED {r['team']} {r['set']} {r['role']}: {r['why']}")
    for w_ in report["warnings"]:
        print(f"  warning: {w_}")


if __name__ == "__main__":
    main()
