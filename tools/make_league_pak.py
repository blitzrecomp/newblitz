#!/usr/bin/env python3
"""Build the "Blitz: Pac-12" league pak (packs/pac12): eight 2026 Pac-12 football schools on NFL base teams,
with fictional players and our own art.

  .venv/bin/python tools/make_league_pak.py [--rom build/teamplus.z64] [--out packs/pac12] [--sheet PNG]

A league pak's teams are built on NFL franchises (their CPU plays, skill, uniforms and 3D models); its
images replace the base teams' files by name, which is safe because no NFL team plays while the pak is
active (src/game/content_pack_tp.cpp, docs/BLITZ2026.md "League paks"). Every image here is drawn from
text and shapes: school initials, wordmarks, a plate, a helmet, end zone lettering, in each school's
colours. The ROM is read only for the base teams' file names, sizes and origins, never for pixels.

Players are fictional (college players' names belong to them), generated from a fixed seed.
"""
import argparse
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

# id, base franchise, city, nickname, select name, abbr, initials, primary, secondary, ratings (pass, rush, line, def, st)
TEAMS = [
    ("BSU", "DEN", "Boise State", "Broncos", "Boise St.", "BSU", "BSU", "#0033A0", "#D64309", (4, 4, 3, 4, 3)),
    ("CSU", "GB", "Colorado State", "Rams", "Colorado St.", "CSU", "CSU", "#1E4D2B", "#C8C372", (3, 3, 2, 3, 3)),
    ("FRE", "ARI", "Fresno State", "Bulldogs", "Fresno St.", "FRE", "FS", "#DB0032", "#13284C", (3, 3, 3, 3, 2)),
    ("ORS", "CIN", "Oregon State", "Beavers", "Oregon St.", "OSU", "OS", "#DC4405", "#111111", (3, 3, 3, 3, 3)),
    ("SDS", "ATL", "San Diego State", "Aztecs", "San Diego St.", "SDS", "SD", "#A6192E", "#111111", (2, 3, 3, 4, 3)),
    ("TXS", "WAS", "Texas State", "Bobcats", "Texas St.", "TXS", "TXST", "#501214", "#B79A5B", (3, 3, 2, 3, 2)),
    ("USU", "DAL", "Utah State", "Aggies", "Utah St.", "USU", "USU", "#0F2439", "#A2AAAD", (3, 2, 2, 2, 3)),
    ("WSU", "TB", "Washington St", "Cougars", "Wash. St.", "WSU", "WSU", "#981E32", "#5E6A71", (4, 2, 3, 3, 3)),
]
NICKNAMES = {"ARI": "Cardinals", "ATL": "Falcons", "CIN": "Bengals", "DAL": "Cowboys", "DEN": "Broncos", "GB": "Packers",
             "TB": "Buccaneers", "WAS": ("Redskins", "Commanders")}

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


def base_files(rom, ents_by_index, franchise):
    """The base team's file names and headers, by role."""
    for k in range(32):
        b = BLOCKS + k * BLOCK
        nick = rom[b + 0x1E0:b + 0x1F0].split(b"\0")[0].decode()
        want = NICKNAMES[franchise]
        if nick in (want if isinstance(want, tuple) else (want,)):
            break
    else:
        raise SystemExit(f"{franchise} isn't in the ROM's team list")
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
    ap.add_argument("--rom", default=str(ROOT / "build" / "teamplus.z64"))
    ap.add_argument("--out", default=str(ROOT / "packs" / "pac12"))
    ap.add_argument("--sheet", default=str(ROOT / "build" / "analysis" / "league" / "pac12_sheet.png"))
    ap.add_argument("--uniforms", help="a folder of finished uniform textures (<ID>/<part>_<home|away>.png at game size, "
                    "build/league_uniforms/README.md): converts them into --out's files/added and points each team at them")
    ap.add_argument("--art", help="a folder of finished art (<ID>/logo.png, logo_small, title_city, title_team, win, helmet, "
                    "endzone at 4x): converts it into --out's files/ instead of drawing. Third-party marks stay out of git: "
                    "point --out at an app folder's packs/pac12 (a user pack wins over the bundled one)")
    a = ap.parse_args(argv)
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
    rng = random.Random(2026)
    teams, written, sheet_rows = [], {}, []
    for (tid, base, city, nick, select, abbr, initials, primary, secondary, ratings) in TEAMS:
        t = dict(id=tid, nickname=nick, initials=initials, primary=primary, secondary=secondary)
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
            "ratings": dict(zip(["passing", "rushing", "linemen", "defense", "special_teams"], ratings)),
            "players": players(t, rng),
        })
    for name, (tid, data) in sorted(written.items()):
        (files / name).write_bytes(data)
    pack = {
        "format": "blitz-pack/2",
        "id": "pac12",
        "title": "Blitz: Pac-12",
        "version": "2026.10.17",
        "requires_rom": "blitz2000-teamplus",
        "league": {
            "name": "Pac-12",
            "modes": ["arcade"],
            "ladder": [["USU", "TXS"], ["CSU", "SDS"], ["ORS", "FRE"], ["WSU", "BSU"]],
            "ladder_done": ["BSU", "WSU", "FRE", "SDS"],
        },
        "teams": teams,
    }
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


def import_uniforms(a):
    """Finished uniform PNGs -> files/added/<id>_<part>_<h|a>.wms and each team's "uniform_files"; clashes from jersey colours."""
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    by_name = fs.by_name(ents)
    src, out = Path(a.uniforms), Path(a.out)
    manifest = json.loads((ROOT / "build" / "league_uniforms" / "manifest.json").read_text())
    pack = json.loads((out / "pack.json").read_text())
    added = out / "files" / "added"
    added.mkdir(parents=True, exist_ok=True)
    colour, n = {}, 0
    for team in pack["teams"]:
        tid = team["id"]
        parts = manifest["teams"].get(tid, {}).get("parts", {})
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
                d = fs.read(rom, by_name[meta["game_file"]])
                typ, w, h, ox, oy = struct.unpack_from(">IIIii", d, 4)
                img = Image.open(png).convert("RGBA")
                if img.size != (w, h):
                    raise SystemExit(f"{png}: {img.size}, the game's is {(w, h)}")
                if part == "helmet_side":
                    # Painted as on the helmet; uniform_handoff.helmet_store turns it into the game's layout
                    # (rows 14-29 flipped, the halves mirror images of each other).
                    from uniform_handoff import helmet_store
                    # The Pac-12 art's logos face right in both halves, so a profile logo's left half is backwards.
                    img = helmet_store(img, "left", profile=tid in PROFILE_LOGOS)
                data = encode_ci4_exact(img, d[3], ox, oy) if typ == 16 else wms.encode_rgba16(img, d[3], ox, oy)
                name = f"{tid.lower()}_{code}_{s}.wms"
                (added / name).write_bytes(data)
                files[key] = name
                n += 1
                if key == "jersey_home":
                    opaque = [p for p in (img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()) if p[3] >= 128]
                    colour[tid] = tuple(sum(p[i] for p in opaque) / len(opaque) for i in range(3))
        if files:
            team["uniform_files"] = files
    # Home jerseys that look alike clash (the second team switches to its away set), as the game's matrix does.
    ids = list(colour)
    for team in pack["teams"]:
        tid = team["id"]
        if tid not in colour:
            continue
        near = [o for o in ids if o != tid and sum((x - y) ** 2 for x, y in zip(colour[tid], colour[o])) ** 0.5 < 70]
        if near:
            team["clashes_with"] = near
        else:
            team.pop("clashes_with", None)
    (out / "pack.json").write_text(json.dumps(pack, indent=1) + "\n")
    print(f"{out}: {n} uniform textures from {src}; clashes: " +
          ", ".join(f"{t['id']}~{'/'.join(t['clashes_with'])}" for t in pack["teams"] if t.get("clashes_with")))


# Schools whose helmet logo is an animal in profile: on the right VS helmet it faces the facemask too, so that
# helmet is the left one mirrored whole. Letter marks (and a front-facing head) read the same on both sides.
PROFILE_LOGOS = {"BSU", "ORS", "FRE", "TXS"}
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


def import_art(a):
    """Finished 4x PNGs -> the pak's .wms files, sized and placed like the drawn set; pack.json copied from packs/pac12."""
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    art, out = Path(a.art), Path(a.out)
    files = out / "files"
    files.mkdir(parents=True, exist_ok=True)
    # The repo's pack.json, unless this folder has one already (it may carry real rosters from college_roster.py):
    # then only the art changes and its players stay.
    if not (out / "pack.json").exists():
        (out / "pack.json").write_text((ROOT / "packs" / "pac12" / "pack.json").read_text())
    n = 0
    for (tid, base, *_rest) in TEAMS:
        primary = _rest[5]
        bf = base_files(rom, ents, base)
        load = lambda name, info: Image.open(art / tid / name).convert("RGBA").resize((info["w"], info["h"]), Image.LANCZOS)
        for role, name in (("logo", "logo.png"), ("logo_small", "logo_small.png"), ("title_city", "title_city.png"),
                           ("title_team", "title_team.png"), ("win", "win.png"), ("vs_left", "helmet.png")):
            info = bf[role]
            img = load(name, info) if role != "title_team" else fit_wordmark(Image.open(art / tid / name).convert("RGBA"), info["w"], info["h"])
            (files / info["name"]).write_bytes(wms.encode_type19(img, info["flags"] or 4, *info["origin"]))
            n += 1
        h, w = bf["endzone1"]["h"], bf["endzone1"]["w"]
        band = Image.open(art / tid / "endzone.png").convert("RGBA").resize((2 * h, w), Image.LANCZOS)
        strip = band.transpose(Image.FLIP_TOP_BOTTOM).rotate(-90, expand=True)
        for role, img in (("endzone1", strip.crop((0, 0, w, h))), ("endzone2", strip.crop((0, h, w, 2 * h))),
                          ("endzone3", Image.new("RGBA", (bf["endzone3"]["w"], bf["endzone3"]["h"]), rgb(primary)))):
            info = bf[role]
            (files / info["name"]).write_bytes(encode_ci4(img, info["flags"], *info["origin"]))
            n += 1
        for role in ("helmet_home", "helmet_away"):
            shell = helmet_shell(bf[role])
            if shell is not None:
                (files / bf[role]["name"]).write_bytes(wms.encode_rgba16(shell, bf[role]["flags"], *bf[role]["origin"]))
        right = art / tid / "helmet_right.png"
        added = files / "added" / f"{tid.lower()}_vsr.wms"
        if right.exists() or tid in PROFILE_LOGOS:
            info = bf["vs_left"]
            added.parent.mkdir(exist_ok=True)
            if tid in PROFILE_LOGOS:
                img = load("helmet.png", info).transpose(Image.FLIP_LEFT_RIGHT)
            else:
                img = Image.open(right).convert("RGBA").resize((info["w"], info["h"]), Image.LANCZOS)
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


if __name__ == "__main__":
    main()
