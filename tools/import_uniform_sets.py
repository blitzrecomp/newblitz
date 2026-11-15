#!/usr/bin/env python3
"""Extra uniform sets (alternates, throwbacks, Color Rush) from finished PNGs into an app folder, for Modern Blitz.

  .venv/bin/python tools/import_uniform_sets.py [--src build/nfl_uniforms_out] [--app-folder build/appdata2026]
                                                [--teams DEN,SEA] [--rom build/baserom.z64]

Reads <src>/<TEAM>/<set>/{helmet_side,...,numbers}.png and set.json ({"name", "dark", ...}), the art round's layout
(build/nfl_uniforms/README.md), and writes <app folder>/uniforms/<TEAM>/<set>/<part>.wms plus set.json. At team
select the Uniform Picker (Settings -> Rules) cycles them after Away. Each part is encoded in the format and size of
the team's own away part, read from your ROM (the plain v1.1 one; Houston's from our 2026 away set); a part left out keeps the team's away one. The helmet side is painted as
it looks on the helmet and turned back into the game's layout here. Everything stays out of git (personal art).
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
from make_league_pak import encode_ci4_exact  # noqa: E402
from uniform_handoff import BLOCK, BLOCKS, NICKNAMES, PARTS, PROFILE_LOGOS, helmet_store  # noqa: E402


def away_parts(rom, ents, tid, app_folder):
    """The team's 10 away uniform files (each part's size and format for the new sets): the plain v1.1 ROM's own
    tables (main 0x8028AF88, 0x28 a team); Houston, which the game lacks, from our 2026 away set; a Team Extension ROM
    (--rom build/teamplus.z64) from its blocks."""
    if rom[0x150C40:0x150C42] == b"zs":
        b = block_of(rom, tid)
        if b is None:
            raise SystemExit(f"{tid} isn't in this ROM")
        return [fs.read(rom, ents[struct.unpack(">H", rom[b + 0x180 + 4 * k + 2:b + 0x180 + 4 * k + 4])[0] - 1]) for k in range(10)]
    if tid == "HOU":
        d = Path(app_folder) / "uniforms" / "HOU" / "01_away_2026"
        return [(d / f"{part}.wms").read_bytes() for part, _ in PARTS]
    want = NICKNAMES[tid] if isinstance(NICKNAMES[tid], tuple) else (NICKNAMES[tid],)
    ob = lambda v: 0xDE350 + v - 0x802EFE70
    main = lambda v: 0x2FC20 + v - 0x80248F00
    for slot in range(31):
        rec = ob(0x8033800C + slot * 0x64)
        if rom[rec + 0x10:rec + 0x30].split(b"\0")[0].decode(errors="replace") in want:
            ids = [struct.unpack(">I", rom[main(0x8028AF88 + slot * 0x28 + 4 * k):main(0x8028AF88 + slot * 0x28 + 4 * k) + 4])[0] & 0xFFFF
                   for k in range(10)]
            return [fs.read(rom, ents[i - 1]) for i in ids]
    raise SystemExit(f"{tid} isn't in this ROM")


def block_of(rom, tid):
    want = NICKNAMES[tid] if isinstance(NICKNAMES[tid], tuple) else (NICKNAMES[tid],)
    return next((BLOCKS + k * BLOCK for k in range(60)
                 if rom[BLOCKS + k * BLOCK + 0x1E0:BLOCKS + k * BLOCK + 0x1F0].split(b"\0")[0].decode(errors="replace") in want), None)


def sixteen(img):
    """At most 15 colours plus transparency, as a 16-colour part holds (art with more is reduced)."""
    img = img.convert("RGBA")
    alpha = img.getchannel("A").point(lambda v: 255 if v >= 128 else 0)
    data = lambda i: list(i.get_flattened_data() if hasattr(i, "get_flattened_data") else i.getdata())
    a = data(alpha)
    if len({p for p, v in zip(data(img.convert("RGB")), a) if v}) + (0 in a) <= 16:
        return img
    rgb = img.convert("RGB").quantize(colors=15, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).convert("RGB")
    out = rgb.convert("RGBA")
    out.putalpha(alpha)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", default=str(ROOT / "build" / "nfl_uniforms_out"))
    ap.add_argument("--app-folder", default=str(ROOT / "build" / "appdata2026"))
    ap.add_argument("--teams", help="comma-separated franchise ids (default: every team folder in --src)")
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"))
    ap.add_argument("--backwards-half", default="right", choices=["right", "left", "none"],
                    help="for helmet art drawn with both halves the same way round: the half facing backwards "
                         "(round 3's logos face left, so its right half; see uniform_handoff.helmet_store)")
    a = ap.parse_args(argv)
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    src, dest = Path(a.src), Path(a.app_folder) / "uniforms"
    teams = [t.strip().upper() for t in a.teams.split(",")] if a.teams else sorted(p.name.upper() for p in src.iterdir() if p.is_dir())
    total = 0
    for tid in teams:
        if tid not in NICKNAMES:
            print(f"{tid}: not a franchise id, skipped")
            continue
        away = away_parts(rom, ents, tid, a.app_folder)
        tdir = next((p for p in src.iterdir() if p.name.upper() == tid), None)
        if not tdir:
            print(f"{tid}: nothing in {src}")
            continue
        # The refreshed bases first (home_2026, away_2026), then the rest by name; a set with the same art as an
        # earlier one is left out (the game would show the same uniform twice).
        first = {"home_2026": 0, "away_2026": 1}
        sets = sorted((p for p in tdir.iterdir() if p.is_dir() and any(p.glob("*.png"))), key=lambda p: (first.get(p.name, 2), p.name))
        seen = {}
        for p in list(sets):
            key = b"".join(f.read_bytes() for f in sorted(p.glob("*.png")))
            if key in seen:
                print(f"{tid}/{p.name}: same art as {seen[key]}, skipped")
                sets.remove(p)
            else:
                seen[key] = p.name
        if len(sets) > 10:
            print(f"{tid}: {len(sets)} sets; the game takes the first 10")
            sets = sets[:10]
        old = dest / tid
        if old.is_dir():  # a re-import replaces the team's sets (folder names carry their order)
            import shutil
            shutil.rmtree(old)
        for n, sdir in enumerate(sets):
            meta = json.loads((sdir / "set.json").read_text()) if (sdir / "set.json").exists() else {}
            out = dest / tid / f"{n:02d}_{sdir.name}"
            out.mkdir(parents=True, exist_ok=True)
            done = []
            for k, (part, _) in enumerate(PARTS):
                png = sdir / f"{part}.png"
                if not png.exists():
                    continue
                d = away[k]  # the away part
                typ, w, h, ox, oy = struct.unpack_from(">IIIii", d, 4)
                img = Image.open(png).convert("RGBA")
                if img.size != (w, h):
                    raise SystemExit(f"{png}: {img.size}, the game's is {(w, h)}")
                if part == "helmet_side":
                    # set.json "helmet_logo": "profile" | "text" | "symmetric"; without it, by team
                    kind = meta.get("helmet_logo") or ("profile" if tid in PROFILE_LOGOS else "text")
                    img = helmet_store(img, a.backwards_half, profile=kind == "profile")
                data = encode_ci4_exact(sixteen(img), d[3], ox, oy) if typ == 16 else wms.encode_rgba16(img, d[3], ox, oy)
                (out / f"{part}.wms").write_bytes(data)
                done.append(part)
            jersey = sdir / "jersey.png"
            dark = meta.get("dark")
            if dark is None and jersey.exists():  # a colour jersey unless it's mostly light
                im = Image.open(jersey).convert("RGBA")
                px = [p for p in (im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata()) if p[3] >= 128]
                dark = sum(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in px) / max(1, len(px)) < 170
            (out / "set.json").write_text(json.dumps({"name": meta.get("name", sdir.name), "dark": bool(dark if dark is not None else True),
                                                      "kind": meta.get("kind", "")}, indent=1) + "\n")
            print(f"{tid}/{sdir.name}: {meta.get('name', sdir.name)} ({len(done)} parts{'' if len(done) == 10 else ', the rest from away'})")
            total += 1
    print(f"{dest}: {total} set(s). Restart the app to load them.")


if __name__ == "__main__":
    main()
