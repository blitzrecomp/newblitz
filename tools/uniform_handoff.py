#!/usr/bin/env python3
"""Uniform templates for an art round: each team's current home and away uniform textures from your ROM, as PNGs.

  .venv/bin/python tools/uniform_handoff.py --teams CIN,DAL,DEN,NYJ,SEA [--rom build/teamplus.z64] [--out build/nfl_uniforms]

Writes <out>/<TEAM>/templates/<part>_<home|away>.png (exact game size), <out>/<TEAM>/preview/ (8x), manifest.json
(sizes, formats, which home/away share a file) and contact_sheet.png. The templates come from your own ROM, so they
and everything made from them stay in build/ (never committed). The helmet side is exported as it looks on the
helmet (the game stores it upside down and wraps both halves the same way round); make_league_pak.py's importer
turns it back.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "announcer"))
import fs  # noqa: E402

BLOCKS, BLOCK = 0x11F17C, 0x220
NICKNAMES = {"ARI": "Cardinals", "ATL": "Falcons", "BAL": "Ravens", "BUF": "Bills", "CAR": "Panthers", "CHI": "Bears",
             "CIN": "Bengals", "CLE": "Browns", "DAL": "Cowboys", "DEN": "Broncos", "DET": "Lions", "GB": "Packers",
             "HOU": "Texans", "IND": "Colts", "JAX": "Jaguars", "KC": "Chiefs", "LV": "Raiders", "LAC": "Chargers",
             "LAR": "Rams", "MIA": "Dolphins", "MIN": "Vikings", "NE": "Patriots", "NO": "Saints", "NYG": "Giants",
             "NYJ": "Jets", "PHI": "Eagles", "PIT": "Steelers", "SF": "49ers", "SEA": "Seahawks", "TB": "Buccaneers",
             "TEN": ("Titans", "Oilers"), "WAS": ("Redskins", "Commanders")}
PARTS = [("helmet_side", "Helmet shell sides with the logo: left half one side, right half the other (shown as on the helmet)"),
         ("helmet_stripe_front", "Centre stripe over the crown, front half"),
         ("helmet_stripe_back", "Centre stripe over the crown, back half"),
         ("facemask", "Facemask"),
         ("shoulder", "Shoulder pads and sleeve caps"),
         ("jersey", "Jersey body, front and back (keep the fold shading)"),
         ("pant_stripe", "Stripe down the pant leg"),
         ("pants", "Pants (keep the fold shading)"),
         ("socks", "Socks"),
         ("numbers", "Number font: the digits 1234567890, stored mirrored")]


def helmet_view(img):
    """Stored helmet side <-> as seen on the helmet: rows 14-29 flipped, the right half's band mirrored (an involution)."""
    img = img.copy()
    w = img.width
    img.paste(img.crop((0, 14, w, 30)).transpose(Image.FLIP_TOP_BOTTOM), (0, 14))
    img.paste(img.crop((w // 2, 14, w, 30)).transpose(Image.FLIP_LEFT_RIGHT), (w // 2, 14))
    return img


def _halves_alike(img):
    """How alike the helmet band's two halves are as drawn (same) and as mirror images (mirrored): -1..1."""
    band = img.convert("L").crop((0, 14, img.width, 30))
    w = img.width // 2
    left, right = list(band.crop((0, 0, w, 16)).getdata()), band.crop((w, 0, 2 * w, 16))
    def corr(x, y):
        mx, my = sum(x) / len(x), sum(y) / len(y)
        sx, sy = sum((a - mx) ** 2 for a in x) ** .5, sum((b - my) ** 2 for b in y) ** .5
        return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (sx * sy) if sx and sy else 0.0
    return corr(left, list(right.getdata())), corr(left, list(right.transpose(Image.FLIP_LEFT_RIGHT).getdata()))


# Helmet logos that are a creature or object in profile, facing the front of the helmet. The two halves of
# the helmet-side texture wrap opposite sides of the helmet, so such a logo has to be a mirror image from one
# half to the other (in helmet view its head points to each half's outer edge, as in the ROM's own helmets).
# Lettering is the opposite: it has to be the same in both halves, or one side reads backwards (the ROM's
# Jets helmet makes that mistake). Symmetric marks (a star, a fleur-de-lis) don't care.
PROFILE_LOGOS = {"ARI", "ATL", "BAL", "BUF", "CAR", "DEN", "DET", "JAX", "LAC", "LAR", "MIA", "MIN", "NE", "PHI",
                 "SEA", "TB", "TEN"}


def helmet_store(img, backwards="right", profile=True):
    """A painted helmet side (as on the helmet) -> the game's layout. For a profile logo drawn the same way round
    in both halves, one half faces backwards: `backwards` names it ("right" when the logos face left, "left" when
    they face right) and it's mirrored first. Lettering (profile=False) is stored as drawn."""
    same, mirrored = _halves_alike(img)
    if profile and same > mirrored + 0.1 and backwards in ("left", "right"):
        img = img.copy()
        w = img.width // 2
        x = 0 if backwards == "left" else w
        img.paste(img.crop((x, 14, x + w, 30)).transpose(Image.FLIP_LEFT_RIGHT), (x, 14))
    return helmet_view(img)


def decode(d):
    typ, w, h, ox, oy = struct.unpack_from(">IIIii", d, 4)
    p = d[32:]
    f = lambda v: (((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31, 255 if v & 1 else 0)
    if typ == 16:
        n = w * h
        # team+'s own textures (~*.wms) can store fewer than 16 palette entries; the rest read as clear.
        pal = [struct.unpack_from(">H", p, n // 2 + 2 * i)[0] if n // 2 + 2 * i + 2 <= len(p) else 0 for i in range(16)]
        px = [f(pal[(p[i // 2] >> (4 if i % 2 == 0 else 0)) & 15]) for i in range(n)]
    else:
        px = [f(struct.unpack_from(">H", p, 2 * i)[0]) for i in range(w * h)]
    im = Image.new("RGBA", (w, h))
    im.putdata(px)
    return im, typ, w, h


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--teams", required=True, help="comma-separated franchise ids, e.g. CIN,DAL")
    ap.add_argument("--rom", default=str(ROOT / "build" / "teamplus.z64"))
    ap.add_argument("--out", default=str(ROOT / "build" / "nfl_uniforms"))
    a = ap.parse_args(argv)
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    out = Path(a.out)
    manifest = {"rom": Path(a.rom).name, "helmet_side": "shown as on the helmet; paint the logo upright. The halves are the two sides of the helmet. A logo in profile (an animal, a bolt) is a mirror image from one half to the other, its head pointing to the outer edge of each half (left in the left half, right in the right), as the templates' animals do. Lettering is identical in both halves (the ROM's Jets helmet has it mirrored; don't copy that)",
                "teams": {}}
    rows = []
    for tid in [t.strip().upper() for t in a.teams.split(",") if t.strip()]:
        want = NICKNAMES[tid] if isinstance(NICKNAMES[tid], tuple) else (NICKNAMES[tid],)
        b = next((BLOCKS + k * BLOCK for k in range(60)
                  if rom[BLOCKS + k * BLOCK + 0x1E0:BLOCKS + k * BLOCK + 0x1F0].split(b"\0")[0].decode(errors="replace") in want), None)
        if b is None:
            raise SystemExit(f"{tid} isn't in {a.rom}")
        d = out / tid
        (d / "templates").mkdir(parents=True, exist_ok=True)
        (d / "preview").mkdir(exist_ok=True)
        entry, seen, row = {"parts": {}}, {}, []
        for k, (part, desc) in enumerate(PARTS):
            for side, sname in ((0, "home"), (1, "away")):
                u = struct.unpack(">H", rom[b + 0x180 + 4 * k + 2 * side:b + 0x180 + 4 * k + 2 * side + 2])[0]
                e = ents[u - 1]
                im, typ, w, h = decode(fs.read(rom, e))
                key = f"{part}_{sname}"
                same = seen.get(e["name"])
                seen.setdefault(e["name"], key)
                entry["parts"][key] = {"game_file": e["name"], "size": [w, h], "description": desc, "same_file_as": same,
                                       "format": "16 colours max, incl. shading and transparency" if typ == 16 else "full colour, 1-bit alpha"}
                if same:
                    continue
                if typ == 8 and not any(px[3] for px in im.getdata()):
                    im.putalpha(255)  # the game ignores alpha on helmet parts; some store none (the Jets'): shown opaque
                    entry["parts"][key]["note"] = "stored with no alpha; the game draws it opaque"
                if part == "helmet_side":
                    im = helmet_view(im)
                im.save(d / "templates" / f"{key}.png")
                im.resize((w * 8, h * 8), Image.NEAREST).save(d / "preview" / f"{key}.png")
                row.append((key, im.resize((w * 4, h * 4), Image.NEAREST)))
        manifest["teams"][tid] = entry
        rows.append((tid, row))
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    sheets = []
    for tid, row in rows:
        h = max(i.height for _, i in row) + 26
        r = Image.new("RGBA", (sum(i.width + 8 for _, i in row) + 60, h), (34, 36, 52, 255))
        dr = ImageDraw.Draw(r)
        dr.text((6, 6), tid, fill=(255, 255, 255, 255))
        x = 60
        for key, i in row:
            r.alpha_composite(i, (x, 22))
            dr.text((x, 6), key.replace("_home", " h").replace("_away", " a")[:18], fill=(200, 200, 200, 255))
            x += i.width + 8
        sheets.append(r)
    sheet = Image.new("RGBA", (max(s.width for s in sheets), sum(s.height for s in sheets)), (34, 36, 52, 255))
    y = 0
    for s in sheets:
        sheet.alpha_composite(s, (0, y))
        y += s.height
    sheet.save(out / "contact_sheet_templates.png")
    print(f"{out}: {len(rows)} teams")


if __name__ == "__main__":
    main()
