#!/usr/bin/env python3
"""The art brief's kit for a team's 2D art (Houston's and the Commanders' to start): for each piece the game shows,
its exact size, format and origin, and the same piece from other teams as a style reference, decoded from your own
ROMs. Everything goes to build/team_art_kit (never commit it: it's the game's art).

  .venv/bin/python tools/team_art_kit.py [--rom build/baserom.z64] [--houston build/engine32/houston]
                                         [--app-folder build/appdata2026]

Writes manifest.json, reference/<team>/<piece>.png (and 4x previews), current/HOU and current/WAS (what plays today,
borrowed: for size and placement only) and a contact sheet.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "announcer"))
import fs  # noqa: E402
import wms  # noqa: E402

REFERENCE = ["Titans", "Ravens", "Broncos", "Jaguars", "Panthers", "Redskins"]
PIECES = {
    "logo": "Team select logo, above the player model (also the VS screen's corner logo)",
    "small_logo": "Small logo: in-game scoreboard and replays",
    "title_city": "City banner on the VS screen (HOUSTON / WASHINGTON lettering)",
    "title_nick": "Nickname lettering on the VS screen (TEXANS / COMMANDERS)",
    "win": "End-of-game winner text (TEXANS WIN style)",
    "vs_left": "VS screen helmet, left side (facing right, toward the VS)",
    "vs_right": "VS screen helmet, right side (facing left). Only some teams have their own; others are mirrored",
    "end_zone_1": "End zone lettering/tile 1 (painted on the field)",
    "end_zone_2": "End zone tile 2",
    "end_zone_3": "End zone tile 3 (a small repeating tile)",
}


def decode(data):
    typ = struct.unpack_from(">I", data, 4)[0]
    if typ == 19:
        try:
            return wms.decode(data)
        except IndexError:  # an alpha plane shorter than the game's own (some community files): colour plane only
            return wms.decode(data[:4] + struct.pack(">I", 8) + data[8:])
    if typ != 16:
        return wms.decode(data)
    flags = data[3]
    _, w, h, ox, oy = struct.unpack_from(">IIIii", data, 4)
    p, n = data[32:], w * h
    pal = [wms.from5551(struct.unpack_from(">H", p, n // 2 + 2 * i)[0]) for i in range(16)]
    px = [pal[(p[i // 2] >> (4 if i % 2 == 0 else 0)) & 15] for i in range(n)]
    if all(c[3] == 0 for c in px):  # the end-zone strips: their palette's alpha bits are clear; the field draws them opaque
        px = [c[:3] + (255,) for c in px]
    img = Image.new("RGBA", (w, h))
    img.putdata(px)
    return img, {"type": typ, "width": w, "height": h, "origin": (ox, oy), "flags": flags}


def team_files(rom, ents):
    """Each original team's file for each piece (the original game's tables)."""
    be = lambda o: struct.unpack(">I", rom[o:o + 4])[0]
    main = lambda v: 0x2FC20 + v - 0x80248F00
    oa = lambda v: 0x93EC0 + v - 0x802EFE70
    ob = lambda v: 0xDE350 + v - 0x802EFE70
    fid = lambda a: (a & 0xFFFF) - 1
    out = {}
    for s in range(31):
        rec = ob(0x8033800C + s * 0x64)
        nick = rom[rec + 0x10:rec + 0x30].split(b"\0")[0].decode()
        tb = be(rec + 0x60)
        r = {"logo": fid(be(rec + 0x5C)), "title_city": fid(be(ob(tb))), "title_nick": fid(be(ob(tb) + 4)),
             "small_logo": fid(be(oa(0x80337280 + 4 * s))), "win": fid(be(oa(0x8033460C + 8 * s))),
             "vs_left": fid(be(main(0x802836E4 + 4 * s)))}
        for k, base in enumerate((0x80335428, 0x803354A4, 0x80335520)):
            r[f"end_zone_{k + 1}"] = fid(be(oa(base + 4 * s)))
        for k in range(10):
            o = main(0x802836AC + 2 * k)
            if struct.unpack(">H", rom[o:o + 2])[0] == s:
                r["vs_right"] = fid(be(main(0x802836C0 + 4 * k)))
        out[nick] = {piece: ents[i]["name"] for piece, i in r.items()}
    return out


ABBR = {"Cardinals": "ARI", "Falcons": "ATL", "Ravens": "BAL", "Bills": "BUF", "Panthers": "CAR", "Bears": "CHI",
        "Bengals": "CIN", "Browns": "CLE", "Cowboys": "DAL", "Broncos": "DEN", "Lions": "DET", "Packers": "GB",
        "Colts": "IND", "Jaguars": "JAX", "Chiefs": "KC", "Raiders": "LV", "Chargers": "LAC", "Rams": "LAR",
        "Dolphins": "MIA", "Vikings": "MIN", "Patriots": "NE", "Saints": "NO", "Giants": "NYG", "Jets": "NYJ",
        "Eagles": "PHI", "Steelers": "PIT", "49ers": "SF", "Seahawks": "SEA", "Buccaneers": "TB", "Titans": "TEN",
        "Redskins": "WAS"}
# The league's marks: game file -> what it is. Only these; the licensing screens (nflpalg, players_sml) stay as they are.
LEAGUE = {
    "vs_nfl.wms": "NFL shield, small: the VS screen's bottom-left corner (three in a row)",
    "nfl122.wms": "NFL shield, large",
    "sp_logo.wms": "NFL shield, medium",
    "nflogo1.wms": "NFL shield, tiny (32x32)",
    "doornfl.wms": "NFL shield embossed on the steel transition doors (keep the door, modernize the shield)",
    "logoafc.wms": "AFC logo: the season standings screen",
    "logonfc.wms": "NFC logo: the season standings screen",
    "nfla.wms": "Midfield NFL shield painted on the turf, left half (grass); the other nfl*.wms tiles are its variants",
    "nflb.wms": "Midfield shield, right half (grass)",
}
MIDFIELD = ["nflaa", "nflab", "nflad", "nflas", "nflba", "nflbas", "nflbb", "nflbd", "nflbs"]


def round6(a, rom, ents, by_name, files, out, save, spec):
    """Every original team (all but Washington, done in round 5), the league's marks and the credits page."""
    app = Path(a.app_folder)
    manifest = {"pieces": PIECES, "teams": {}, "league": {}, "credits": {}}
    for nick, pieces in files.items():
        abbr = ABBR[nick]
        if abbr == "WAS":
            continue
        team = {}
        for piece, name in pieces.items():
            data = fs.read(rom, by_name[name])
            img, _ = decode(data)
            save(img, out / "reference" / abbr / f"{piece}.png")
            over = app / "art" / "files" / name
            shown = over.read_bytes() if over.exists() else data
            img2, _ = decode(shown)
            save(img2, out / "current" / abbr / f"{piece}.png")
            team[piece] = {"game_file": name, "deliver_as": f"{abbr}/{piece}.png",
                           "today": "already replaced by the app (keep unless you redo it)" if over.exists() else "the 1999 art",
                           **spec(data)}
        manifest["teams"][abbr] = team
        # The team's 2026 sets: the helmets the VS screen should match.
        for d in sorted((app / "uniforms" / abbr).glob("*")):
            for part in ("helmet_side", "jersey"):
                f = d / f"{part}.wms"
                if f.exists():
                    img, _ = decode(f.read_bytes())
                    save(img, out / "uniforms_2026" / abbr / f"{d.name}_{part}.png")
    for name, what in LEAGUE.items():
        data = fs.read(rom, by_name[name])
        img, _ = decode(data)
        save(img, out / "league" / name.replace(".wms", ".png"))
        manifest["league"][name.replace(".wms", "")] = {"game_file": name, "what": what, "deliver_as": f"league/{name.replace('.wms', '.png')}", **spec(data)}
    for n in MIDFIELD:
        data = fs.read(rom, by_name[n + ".wms"])
        img, _ = decode(data)
        save(img, out / "league" / "midfield_variants" / f"{n}.png")
        manifest["league"][n] = {"game_file": n + ".wms", "what": "midfield shield tile variant (another surface)",
                                 "deliver_as": f"league/{n}.png", **spec(data)}
    data = fs.read(rom, by_name["nflpalg.wms"])
    img, _ = decode(data)
    save(img, out / "credits" / "style_reference_licensing_screen.png")
    manifest["credits"] = {"deliver_as": "credits/credits.png", "width": 512, "height": 384,
                           "what": "One full-screen BlitzRecomp credits page, shown at startup after the game's own screens"}
    (out / "manifest_round6.json").write_text(json.dumps(manifest, indent=1))
    print(f"-> {out}: manifest_round6.json, reference/, current/, uniforms_2026/, league/, credits/")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    ap.add_argument("--houston", default=str(ROOT / "build" / "engine32" / "houston"))
    ap.add_argument("--app-folder", default=str(ROOT / "build" / "appdata2026"))
    ap.add_argument("--out", default=str(ROOT / "build" / "team_art_kit"))
    ap.add_argument("--round6", action="store_true", help="every team, the league's marks and the credits page")
    a = ap.parse_args()
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    by_name = fs.by_name(ents)
    out = Path(a.out)
    files = team_files(rom, ents)
    manifest = {"pieces": PIECES, "reference_teams": {}, "deliver": {}}

    def save(img, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        img.save(path)
        img.resize((img.width * 4, img.height * 4), Image.NEAREST).save(path.with_name(path.stem + "_4x.png"))

    def spec(data):
        _, hdr = decode(data)
        fmt = {0: "256 colours (8-bit index)", 8: "16-bit colour, 1-bit alpha", 16: "16 colours (4-bit index)",
               19: "16-bit colour + 4-bit alpha (soft edges)"}[hdr["type"]]
        return {"width": hdr["width"], "height": hdr["height"], "origin": list(hdr["origin"]), "flags": hdr["flags"],
                "wms_type": hdr["type"], "format": fmt}

    if a.round6:
        return round6(a, rom, ents, by_name, files, out, save, spec)

    for team in REFERENCE:
        manifest["reference_teams"][team] = {}
        for piece, name in files[team].items():
            data = fs.read(rom, by_name[name])
            img, _ = decode(data)
            save(img, out / "reference" / team / f"{piece}.png")
            manifest["reference_teams"][team][piece] = {"file": name, **spec(data)}

    # Houston: the Team Extension's files (block.bin's ids), as engine32 plays them today.
    block = (Path(a.houston) / "block.bin").read_bytes()
    hou_ids = {"logo": 0x1DA, "small_logo": 0x1D8, "title_city": 0x1DC, "title_nick": 0x1DE, "win": 0x21A,
               "vs_left": 0x21C, "vs_right": 0x21E, "end_zone_1": 0x214, "end_zone_2": 0x216, "end_zone_3": 0x218}
    hou = {}
    for piece, off in hou_ids.items():
        idx = struct.unpack(">H", block[off:off + 2])[0] - 1
        f = Path(a.houston) / f"f{idx}.wms"
        data = f.read_bytes() if f.exists() else fs.read(rom, ents[idx])
        img, _ = decode(data)
        save(img, out / "current" / "HOU" / f"{piece}.png")
        hou[piece] = {"deliver_as": f"HOU/{piece}.png", **spec(data)}
    manifest["deliver"]["HOU"] = hou

    # The Commanders: Washington's game files, replaced by name (art drop-in); today's borrowed versions if present.
    was = {}
    for piece, name in files["Redskins"].items():
        data = fs.read(rom, by_name[name])
        borrowed = Path(a.app_folder) / "art" / "files" / name
        shown = borrowed.read_bytes() if borrowed.exists() else data
        img, _ = decode(shown)
        save(img, out / "current" / "WAS" / f"{piece}.png")
        s = spec(shown)
        was[piece] = {"deliver_as": f"WAS/{piece}.png", "replaces_game_file": name,
                      "currently": "borrowed (Resurrection)" if borrowed.exists() else "the 1999 Redskins art (keep for Classic)",
                      **s}
    manifest["deliver"]["WAS"] = was
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))

    # Contact sheet: each piece across the reference teams and today's HOU / WAS.
    cols = REFERENCE + ["HOU now", "WAS now"]
    rows = list(PIECES)
    cell_w, cell_h = 330, 130
    sheet = Image.new("RGBA", (160 + cell_w * len(cols), 30 + cell_h * len(rows)), (40, 40, 48, 255))
    from PIL import ImageDraw
    d = ImageDraw.Draw(sheet)
    for c, name in enumerate(cols):
        d.text((160 + c * cell_w + 4, 8), name, fill=(255, 255, 255, 255))
    for r, piece in enumerate(rows):
        d.text((4, 30 + r * cell_h + 4), piece, fill=(255, 255, 255, 255))
        for c, team in enumerate(cols):
            p = (out / "current" / team.split()[0] / f"{piece}.png") if " now" in team else (out / "reference" / team / f"{piece}.png")
            if not p.exists():
                continue
            img = Image.open(p)
            img.thumbnail((cell_w - 10, cell_h - 10))
            sheet.alpha_composite(img, (160 + c * cell_w + 4, 30 + r * cell_h + 4))
    sheet.save(out / "contact_sheet.png")
    print(f"-> {out}: manifest.json, reference/, current/, contact_sheet.png")


if __name__ == "__main__":
    main()
