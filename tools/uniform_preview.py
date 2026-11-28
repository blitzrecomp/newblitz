#!/usr/bin/env python3
"""Preview a uniform set on the game's own player model, without running the game.

  .venv/bin/python tools/uniform_preview.py FOLDER [--out sheet.png] [--number 18] [--reference] [--set NAME]

FOLDER is any of:
  - an NFL art round's set: <TEAM>/<set>/ with helmet_side.png ... numbers.png and set.json (build/nfl_uniforms_out);
    an art round's team folder (<TEAM>/ with set folders) previews every set;
  - a league handoff team: <ID>/ with <part>_home.png, <part>_away.png, uniform_sets.json and alternates/<set>/
    (the 2026 Pac-12 and CFL handoffs) previews home, away and each alternate;
  - an installed set: a folder of <part>.wms (an app folder's uniforms/<TEAM>/<set>, a league pak's files).

PNGs go through the importer itself (tools/import_uniform_sets.py for NFL sets, tools/make_league_pak.py for league
teams) into a scratch folder, and the .wms files it writes are what is drawn: a flip, a half the wrong way round or
a palette squeezed to 16 colours shows exactly as the game will show it. The jersey gets its numbers drawn in as
main func_80256580 does it (digits from the numbers part, at x 8/18 or 13, y 8; their colours take jersey palette
entries 13-15), and every triangle samples its part through the game's own RDP tile (wrap, mirror, clamp, filter).

The model is build/uniform_mannequin/mannequin.json, made once from an RDRAM dump of your own game by
tools/make_mannequin.py (docs/UNIFORM_PREVIEW.md). The sheet shows the player front, three-quarter, left, back and
right; both helmet sides at game size and at 4x (nearest neighbour); the numbers as worn; the pants from the side;
the converted textures; and with --reference the NFL team the set is based on.
"""
import argparse
import json
import math
import shutil
import struct
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "announcer"))
import gbi_dump as g  # noqa: E402  (it takes tools/ off sys.path: tools/dis.py shadows the stdlib's)
sys.path.append(str(ROOT / "tools"))

PARTS = ["helmet_side", "helmet_stripe_front", "helmet_stripe_back", "facemask", "shoulder", "jersey", "pant_stripe",
         "pants", "socks", "numbers"]
MANNEQUIN = ROOT / "build" / "uniform_mannequin" / "mannequin.json"
BG = (44, 46, 58, 255)
PANEL = (64, 70, 84, 255)
INK = (235, 235, 240, 255)
DIM = (170, 174, 190, 255)


# ---------------------------------------------------------------------------------------------------------------
# Uniform parts (.wms), and the jersey's numbers as the game draws them
# ---------------------------------------------------------------------------------------------------------------

def c5551(v):
    return g.rgba5551(v)


class Part:
    """A decoded uniform part: size, and either 16-bit pixels or 4-bit indices with a 16-entry palette."""

    def __init__(self, data):
        typ, w, h, ox, oy = struct.unpack_from(">IIIii", data, 4)
        self.typ, self.w, self.h, self.data = typ, w, h, data
        body = data[32:]
        if typ == 16:
            n = w * h
            self.idx = [(body[i // 2] >> (4 if i % 2 == 0 else 0)) & 15 for i in range(n)]
            self.pal = [struct.unpack_from(">H", body, n // 2 + 2 * i)[0] if n // 2 + 2 * i + 2 <= len(body) else 0
                        for i in range(16)]
            self.px = None
        else:
            self.idx = self.pal = None
            self.px = [struct.unpack_from(">H", body, 2 * i)[0] for i in range(w * h)]

    def rgba(self):
        if self.px is not None:
            return [c5551(v) for v in self.px]
        return [c5551(self.pal[i]) for i in self.idx]

    def image(self):
        im = Image.new("RGBA", (self.w, self.h))
        im.putdata(self.rgba())
        return im


def jersey_with_number(jersey, numbers, number):
    """main func_80256580 + boot func_800124D8: the jersey copied into a player slot's buffer, then each digit of the
    (two-digit) number blitted from the numbers strip: 12 px per digit, "1234567890" left to right, 16 rows; tens at
    (8, 8) and units at (18, 8), or a single digit at (13, 8). A digit pixel is drawn where its numbers-palette colour
    is opaque, as index remap[i] (0 -> 0, 1-3 -> 13-15, else 0), and numbers palette entries 1-3 replace the jersey
    palette's entries 13-15. Returns (indices, palette)."""
    idx = list(jersey.idx)
    pal = list(jersey.pal)
    pal[13:16] = numbers.pal[1:4]
    remap = [0, 13, 14, 15] + [0] * 12
    tens, ones = (number // 10) % 10, number % 10
    digits = [(8, tens), (18, ones)] if tens else [(13, ones)]
    for dx, d in digits:
        sx0 = (d - 1) * 12 if d else 108
        for row in range(16):
            for k in range(12):
                v = numbers.idx[row * numbers.w + sx0 + k]
                if numbers.pal[v] & 1:
                    x, y = dx + k, 8 + row
                    if 0 <= x < jersey.w and 0 <= y < jersey.h:
                        idx[y * jersey.w + x] = remap[v]
    return idx, pal


class Uniform:
    def __init__(self, label, files, notes=None):
        """files: part -> .wms bytes (all ten)."""
        self.label = label
        self.parts = {p: Part(files[p]) for p in PARTS if p in files}
        self.notes = notes or []
        self.number = 18
        self._cache = {}
        # A league pak's per-set override (src/game/uv_overrides.cpp): per mannequin draw, a part's texture of its own
        # (a cap's companion, or the set's override texture), and the mannequin's triangles with the table's S/T.
        self.draw_parts = {}   # draw -> {part: Part}
        self.tris = None

    def texels_for(self, part, draw):
        """A part's texels as draw `draw` samples them (a cap with a texture of its own samples that)."""
        own = self.draw_parts.get(draw, {}).get(part)
        if own is None:
            return self.texels(part)
        key = (part, draw)
        if key not in self._cache:
            self._cache[key] = own.rgba()
        return self._cache[key]

    def texels(self, part):
        """RGBA tuples of a part as the game samples it (the jersey with its numbers drawn in)."""
        if part not in self._cache:
            p = self.parts[part]
            if part == "jersey" and "numbers" in self.parts:
                idx, pal = jersey_with_number(p, self.parts["numbers"], self.number)
                self._cache[part] = [c5551(pal[i]) for i in idx]
            else:
                self._cache[part] = p.rgba()
        return self._cache[part]

    def size(self, part):
        p = self.parts[part]
        return p.w, p.h

    def jersey_image(self):
        w, h = self.size("jersey")
        im = Image.new("RGBA", (w, h))
        im.putdata(self.texels("jersey"))
        return im


# ---------------------------------------------------------------------------------------------------------------
# Loading: installed .wms sets, NFL art-round PNG sets (through import_uniform_sets), league teams (make_league_pak)
# ---------------------------------------------------------------------------------------------------------------

def _rom(path):
    import fs
    rom = fs.load(path)
    return rom, fs.toc(rom)[1]


def nfl_away_files(tid, app_folder):
    """The team's ROM away parts (what the game keeps for a part a set leaves out)."""
    from import_uniform_sets import away_parts
    rom, ents = _rom(find_rom(teamplus=False))
    return dict(zip(PARTS, away_parts(rom, ents, tid, app_folder)))


def installed_set(folder, team=None, app_folder=None):
    files = {p: (folder / f"{p}.wms").read_bytes() for p in PARTS if (folder / f"{p}.wms").exists()}
    notes = []
    if len(files) < 10 and team:
        away = nfl_away_files(team, app_folder or ROOT / "build" / "appdata2026")
        for p in PARTS:
            if p not in files:
                files[p] = away[p]
                notes.append(f"{p}: the team's away part (not in the set)")
    meta = json.loads((folder / "set.json").read_text()) if (folder / "set.json").exists() else {}
    return Uniform(meta.get("name", folder.name), files, notes)


def nfl_png_sets(team_dir, sets, tid, scratch):
    """Run tools/import_uniform_sets.py on the sets (into a scratch app folder) and load what it wrote."""
    from import_uniform_sets import main as import_sets
    src = scratch / "src" / tid
    src.mkdir(parents=True, exist_ok=True)
    for s in sets:
        (src / s.name).symlink_to(s.resolve())
    app = scratch / "app"
    log = scratch / "import.log"
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        import_sets(["--src", str(scratch / "src"), "--app-folder", str(app), "--teams", tid,
                     "--rom", str(find_rom(teamplus=False))])
    log.write_text(buf.getvalue())
    out = []
    for d in sorted((app / "uniforms" / tid).iterdir()):
        u = installed_set(d, tid)
        u.notes = [line for line in buf.getvalue().splitlines() if f"/{d.name[3:]}:" in line] + u.notes
        out.append(u)
    return out


def find_rom(teamplus):
    """The ROM the importers read part sizes and base files from (build/teamplus.z64 for league paks,
    build/baserom.z64 for NFL sets), else the copy a dev run stores in build/appdata2026; None if neither."""
    names = (["build/teamplus.z64", "build/appdata2026/blitz2000.teamplus.n64.us.z64"] if teamplus else
             ["build/baserom.z64", "build/appdata2026/blitz2000.n64.us.1.1.z64"])
    for n in names:
        if (ROOT / n).exists():
            return ROOT / n
    raise SystemExit(f"no {'Team Extension ' if teamplus else ''}ROM: looked for {' and '.join(names)}")


def _run_league_import(tid, team_dir, scratch, rom_path, part_files=None):
    """make_league_pak.import_uniforms on one team (into a scratch pak). part_files: {<part>_home: png} instead of the
    team folder (an alternate, imported as the importer imports a home set). -> (team, files dir, base game files)."""
    import contextlib
    import io
    import make_league_pak as mlp
    src = scratch / "src"
    src.mkdir(parents=True, exist_ok=True)
    if part_files is None:
        (src / tid).symlink_to(team_dir.resolve())
    else:
        (src / tid).mkdir()
        for key, png in part_files.items():
            (src / tid / f"{key}.png").symlink_to(Path(png).resolve())
    out = scratch / "pak"
    out.mkdir(parents=True, exist_ok=True)
    pack = None
    for p in (ROOT / "packs").glob("*/pack.json"):
        pj = json.loads(p.read_text())
        if any(t.get("id") == tid for t in pj.get("teams", [])):
            pack = pj
            break
    pack = pack or {"teams": [{"id": tid}]}
    pack["teams"] = [t for t in pack["teams"] if t.get("id") == tid]
    (out / "pack.json").write_text(json.dumps(pack))
    args = argparse.Namespace(rom=str(rom_path), uniforms=str(src), out=str(out), art=None, sheet=None, kit=None)
    # Base game file per part: the design kit's parts.json, else the older Pac-12 manifest.
    kits = ROOT / "build" / "league_pak_design_kit"
    kit = next((kits / lg for lg in ("pac12", "cfl") if (kits / lg / tid / "parts.json").exists()), None)
    manifest_path = ROOT / "build" / "league_uniforms" / "manifest.json"
    parts = {}
    if kit:
        parts = json.loads((kit / tid / "parts.json").read_text())
        game_file = {k: v["base_game_file"] for k, v in parts.items()}
    elif manifest_path.exists():
        game_file = {k: v["game_file"] for k, v in
                     json.loads(manifest_path.read_text())["teams"].get(tid, {}).get("parts", {}).items()}
    else:
        game_file = {}
    if not game_file:
        raise SystemExit(f"{tid}: no base part list (build/league_pak_design_kit/<league>/{tid}/parts.json)")
    if hasattr(mlp, "load_league"):
        # The data-driven importer (cfl-league) reads its league config (tools/leagues/<id>.json) from args.cfg.
        args.cfg = mlp.load_league(kit.name if kit else "pac12")
    saved_root = mlp.ROOT
    if kit and "kit" in mlp.import_uniforms.__code__.co_names:
        args.kit = str(kit)
    elif kit:
        # This checkout's importer reads ROOT/build/league_uniforms/manifest.json only: give it one made from the kit.
        fake = scratch / "root"
        (fake / "build" / "league_uniforms").mkdir(parents=True, exist_ok=True)
        (fake / "build" / "league_uniforms" / "manifest.json").write_text(json.dumps(
            {"teams": {tid: {"parts": {k: dict(v, game_file=v["base_game_file"]) for k, v in parts.items()}}}}))
        mlp.ROOT = fake
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            mlp.import_uniforms(args)
    finally:
        mlp.ROOT = saved_root
    (scratch / "import.log").write_text(buf.getvalue())
    team = json.loads((out / "pack.json").read_text())["teams"][0]
    return team, out / "files" / "added", game_file


def league_team(team_dir, scratch, rom_path=None):
    """Run tools/make_league_pak.py's uniform import on one handoff team (into a scratch pak) and load home, away and
    each alternate. Parts the art leaves out are the base team's (the game keeps them)."""
    import fs
    tid = team_dir.name.upper()
    rom_path = rom_path or find_rom(teamplus=True)
    team, files_dir, game_file = _run_league_import(tid, team_dir, scratch / "main", rom_path)
    rom, ents = _rom(rom_path)
    by_name = fs.by_name(ents)

    def files_for(names, side, fdir):
        files, notes = {}, []
        for p in PARTS:
            n = names.get(p)
            if n and (fdir / n).exists():
                files[p] = (fdir / n).read_bytes()
            elif f"{p}_{side}" in game_file:
                files[p] = fs.read(rom, by_name[game_file[f"{p}_{side}"].lower()])
                notes.append(f"{p}: the base team's (not in the art)")
        return files, notes

    out_sets = []
    uf = team.get("uniform_files", {})
    for side in ("home", "away"):
        files, notes = files_for({p: uf.get(f"{p}_{side}") for p in PARTS}, side, files_dir)
        if len(files) == 10:
            out_sets.append(Uniform(side, files, notes))
    meta = json.loads((team_dir / "uniform_sets.json").read_text()) if (team_dir / "uniform_sets.json").exists() else {}
    alts = meta.get("alternates_delivered", [])
    if not alts and (team_dir / "alternates").is_dir():
        alts = [{"set_id": p.name} for p in sorted((team_dir / "alternates").iterdir()) if p.is_dir()]
    if team.get("uniform_sets"):
        # An importer that takes alternates itself: what it wrote.
        for k, s in enumerate(team["uniform_sets"]):
            names = s.get("files", s.get("uniform_files", {}))
            files, notes = files_for({p: names.get(p) for p in PARTS}, s.get("template_basis", "home"), files_dir)
            if len(files) == 10:
                out_sets.append(Uniform(s.get("name", f"alternate {k + 1}"), files, notes))
    else:
        # This checkout's importer takes home and away only: each alternate goes through it as a home set.
        for k, alt in enumerate(alts):
            folder = team_dir / alt.get("directory", f"alternates/{alt['set_id']}")
            pngs = {f"{p}_home": folder / f"{p}.png" for p in PARTS if (folder / f"{p}.png").exists()}
            if not pngs:
                continue
            at, adir, _ = _run_league_import(tid, team_dir, scratch / f"alt{k}", rom_path, pngs)
            auf = at.get("uniform_files", {})
            files, notes = files_for({p: auf.get(f"{p}_home") for p in PARTS}, alt.get("template_basis", "home"), adir)
            if len(files) == 10:
                out_sets.append(Uniform(alt.get("name") or alt["set_id"], files, notes))
    if not out_sets:
        raise SystemExit(f"{team_dir}: the importer wrote no uniform for {tid}")
    return out_sets, team


def reference_uniform(franchise, app_folder):
    """The NFL team's look: its installed 2026 home set if the app folder has one, else its ROM home parts."""
    if app_folder:
        for base in (Path(app_folder) / "uniforms" / franchise,):
            homes = sorted(base.glob("00_*")) if base.is_dir() else []
            if homes:
                u = installed_set(homes[0], franchise, app_folder)
                u.label = f"{franchise} {u.label} (reference)"
                return u
    import fs
    from uniform_handoff import BLOCK, BLOCKS, NICKNAMES
    rom, ents = _rom(find_rom(teamplus=True))
    want = NICKNAMES[franchise] if isinstance(NICKNAMES[franchise], tuple) else (NICKNAMES[franchise],)
    b = next(BLOCKS + k * BLOCK for k in range(60)
             if rom[BLOCKS + k * BLOCK + 0x1E0:BLOCKS + k * BLOCK + 0x1F0].split(b"\0")[0].decode(errors="replace") in want)
    files = {p: fs.read(rom, ents[struct.unpack(">H", rom[b + 0x180 + 4 * k:b + 0x182 + 4 * k])[0] - 1])
             for k, p in enumerate(PARTS)}
    return Uniform(f"{franchise} home (ROM, reference)", files)


def apply_override(u, ref, table, man):
    """A league pak set's "uv_override" on the mannequin: per-cap textures on the part's draws (in block order, the
    caps of tools/uv_override.py's PARTS), the set's override texture on the rest, and the table's coordinates on
    the sampled model's block (model 0, the team-select player)."""
    import uv_override as uvo
    part = table["part"]
    fdir = u.files_dir
    draws = sorted(d for d, e in enumerate(man.draws) if part_of(e) == [part])
    caps = dict(zip(uvo.PARTS[part]["caps"], draws))
    for d in draws:
        if ref.get("file"):
            u.draw_parts.setdefault(d, {})[part] = Part((fdir / ref["file"]).read_bytes())
    for cap, f in ref.get("caps", {}).items():
        if cap not in caps:
            u.notes.append(f"cap {cap}: not one of the model's {part} draws")
            continue
        u.draw_parts.setdefault(caps[cap], {})[part] = Part((fdir / f).read_bytes())
        u.notes.append(f"{cap} cap ({'draw %d' % caps[cap]}): {f}")
    # Coordinates: the sampled model's block (the one whose first draw's first triangle is the mannequin's draw).
    rom = uvo.Rom(find_rom(teamplus=False))
    blk = next((b for b in uvo.blocks_of(rom, rom.u32(rom.u32(uvo.MODEL_TABLE) + 8), uvo.PARTS[part]["tile"])), None)
    mesh = next((m for m in table["meshes"] if blk and m["block"] == f"{blk['block']:08X}"), None)
    if not mesh or all(v["uv"] == v["st"] for v in mesh["vertices"]):
        return
    ss, ts = blk["scale"]
    to_raw = (32 * 65536 / ss, 32 * 65536 / ts)
    new_tris = []
    moved = 0
    for d, vs in man.tris:
        k = draws.index(d) if d in draws else -1
        if k < 0 or k >= len(mesh["vertices"]):
            new_tris.append((d, vs))
            continue
        ref_v = mesh["vertices"][k]
        nv = []
        for p, s, t, sh in vs:
            raw = (s * to_raw[0], t * to_raw[1])
            j = min(range(len(ref_v["st"])), key=lambda i: abs(ref_v["st"][i][0] - raw[0]) + abs(ref_v["st"][i][1] - raw[1]))
            nv.append((p, ref_v["uv"][j][0] / to_raw[0], ref_v["uv"][j][1] / to_raw[1], sh))
            moved += 1
        new_tris.append((d, nv))
    u.tris = new_tris
    u.notes.append(f"{table.get('_name', 'table')}: {moved} corners on the table's coordinates")


def pack_team(pack_dir, team, man=None):
    """A built league pak's team (<pack folder>/pack.json + files/added): home, away and each alternate as the app
    dresses them, with each set's "uv_override" (per-cap textures, coordinates) applied. Parts a set leaves out are
    the base team's away ones (the game keeps them)."""
    import fs
    import make_league_pak as mlp
    pack = json.loads((pack_dir / "pack.json").read_text())
    t = next((x for x in pack["teams"] if x["id"].upper() == (team or "").upper()), None)
    if t is None:
        raise SystemExit(f"{pack_dir}: no team {team} (--team one of {', '.join(x['id'] for x in pack['teams'])})")
    rom_path = find_rom(teamplus=True)
    rom, ents = _rom(rom_path)
    base = mlp.base_uniform_parts(rom, ents, t["base"])
    by_name = {e["name"].lower(): e for e in ents}
    fdir = pack_dir / "files" / "added"
    uf = t.get("uniform_files", {})

    def files(names, side):
        out, notes = {}, []
        for k, p in enumerate(PARTS):
            n = names.get(p) or (uf.get(f"{p}_away") if side == "alt" else None)
            if n and (fdir / n).exists():
                out[p] = (fdir / n).read_bytes()
            else:
                e = by_name[base[k][0 if side == "home" else 1]["name"]]
                out[p] = fs.read(rom, e)
                notes.append(f"{p}: the base team's")
        return out, notes

    sets = []
    ov = t.get("uv_override", {})
    for side in ("home", "away"):
        f, notes = files({p: uf.get(f"{p}_{side}") for p in PARTS}, side)
        sets.append((side, f, notes, ov.get(side)))
    for s in t.get("uniform_sets", []):
        if s["id"] in ("home", "away"):
            continue
        f, notes = files(s.get("files", {}), "alt")
        sets.append((s["id"], f, notes, s.get("uv_override")))
    out = []
    tables = pack.get("uv_overrides", {})
    for label, f, notes, ref in sets:
        u = Uniform(f"{t['id']} {label}", f, notes)
        u.files_dir = fdir
        if ref and man is not None:
            table = dict(tables[ref["table"]], _name=ref["table"])
            apply_override(u, ref, table, man)
        out.append(u)
    return out, t["base"]


def load(folder, scratch, team=None, man=None):
    """-> (list of Uniform, base franchise or None)."""
    folder = Path(folder)
    if (folder / "pack.json").exists():
        return pack_team(folder, team, man)
    if any(folder.glob("*.wms")):
        tid = (team or folder.parent.name).upper()
        from uniform_handoff import NICKNAMES
        return [installed_set(folder, tid if tid in NICKNAMES else None)], (tid if tid in NICKNAMES else None)
    if (folder / "jersey_home.png").exists() or (folder / "uniform_sets.json").exists():
        sets, t = league_team(folder, scratch)
        return sets, t.get("base")
    if any(folder.glob("*.png")):
        tid = (team or folder.parent.name).upper()
        return nfl_png_sets(folder.parent, [folder], tid, scratch), tid
    subs = sorted(p for p in folder.iterdir() if p.is_dir() and any(p.glob("*.png")))
    if subs:
        tid = (team or folder.name).upper()
        return nfl_png_sets(folder, subs, tid, scratch), tid
    raise SystemExit(f"{folder}: no uniform here (see --help for the folder layouts)")


# ---------------------------------------------------------------------------------------------------------------
# The mannequin and a small software rasteriser (perspective, z-buffer, the RDP tile, point or bilinear)
# ---------------------------------------------------------------------------------------------------------------

class Mannequin:
    def __init__(self, path=MANNEQUIN):
        if not Path(path).exists():
            raise SystemExit(f"no mannequin at {path}: make it once with tools/make_mannequin.py (docs/UNIFORM_PREVIEW.md)")
        m = json.loads(Path(path).read_text())
        self.m = m
        self.draws = m["draws"]
        f = m["frame"]
        o, up, fr = f["origin"], f["up"], f["front"]
        left = [up[1] * fr[2] - up[2] * fr[1], up[2] * fr[0] - up[0] * fr[2], up[0] * fr[1] - up[1] * fr[0]]
        self.axes = (left, up, fr)
        # Triangles in the body frame: x to the player's left, y up, z out of the facemask; origin at the helmet.
        self.tris = []
        for t in m["tris"]:
            d = t[0]
            vs = []
            for v in t[1:]:
                p = [v[0] - o[0], v[1] - o[1], v[2] - o[2]]
                vs.append(([sum(p[k] * ax[k] for k in range(3)) for ax in self.axes], v[3], v[4], v[5:9]))
            self.tris.append((d, vs))
        self.view_tris = [(t[0], [(v[:3], v[3], v[4], v[5:9]) for v in t[1:]]) for t in m["tris"]]

    def centre(self, parts):
        pts = [v[0] for d, vs in self.tris if set(self.draws[d].get("parts", [])) & set(parts) for v in vs]
        if not pts:
            return [0.0, 0.0, 0.0], 1.0
        c = [(min(p[k] for p in pts) + max(p[k] for p in pts)) / 2 for k in range(3)]
        r = max(math.dist(p, c) for p in pts)
        return c, r


def part_of(entry):
    return entry.get("parts", [])


class Sampler:
    """One draw's texture: tile texel (s, t) -> RGBA, through the mannequin's texel map and the uniform's parts."""

    def __init__(self, entry, uniform, part_ids, dump_uniform=None, draw=None):
        self.e = entry
        self.on = bool(entry.get("tex_on"))
        if not self.on:
            return
        tl = entry["tile"]
        self.w, self.h = entry["size"]
        self.tl = tl
        self.cache = {}
        texels = entry["texels"]
        fixed = entry["fixed"]
        out = []
        for i, src in enumerate(texels):
            if src is None:
                v = fixed[i]
                out.append(((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255))
            else:
                pid, x, y = src
                part = part_ids[pid]
                if dump_uniform is not None:
                    du = dump_uniform[part]
                    out.append(tuple(du["rgba"][y * du["w"] + x]))
                else:
                    pw, ph = uniform.size(part)
                    out.append(uniform.texels_for(part, draw)[y * pw + x] if x < pw and y < ph else (255, 0, 255, 255))
        self.tex = out
        self.ul_s, self.ul_t = tl["uls"] / 4.0, tl["ult"] / 4.0
        self.lr_s, self.lr_t = (tl["lrs"] >> 2) - (tl["uls"] >> 2), (tl["lrt"] >> 2) - (tl["ult"] >> 2)

    @staticmethod
    def _shift(v, sh):
        if 0 < sh <= 10:
            return v / (1 << sh)
        if sh > 10:
            return v * (1 << (16 - sh))
        return v

    def _coord(self, i, mask, cm, lr):
        if (cm & 2) or mask == 0:
            i = 0 if i < 0 else (lr if i > lr else i)
        if mask:
            if (cm & 1) and (i >> mask) & 1:
                i = ~i
            i &= (1 << mask) - 1
        return i

    def texel(self, si, ti):
        tl = self.tl
        s = self._coord(si, tl["masks"], tl["cms"], self.lr_s)
        t = self._coord(ti, tl["maskt"], tl["cmt"], self.lr_t)
        if s >= self.w or t >= self.h:
            s, t = min(s, self.w - 1), min(t, self.h - 1)
        return self.tex[t * self.w + s]

    def sample(self, s, t, bilinear):
        tl = self.tl
        s = self._shift(s, tl["shifts"]) - self.ul_s
        t = self._shift(t, tl["shiftt"]) - self.ul_t
        if not bilinear:
            return self.texel(math.floor(s), math.floor(t))
        s -= 0.5
        t -= 0.5
        i, j = math.floor(s), math.floor(t)
        fs, ft = s - i, t - j
        a, b = self.texel(i, j), self.texel(i + 1, j)
        c, d = self.texel(i, j + 1), self.texel(i + 1, j + 1)
        return tuple((a[k] * (1 - fs) + b[k] * fs) * (1 - ft) + (c[k] * (1 - fs) + d[k] * fs) * ft for k in range(4))


def combine(entry, tex, shade):
    """The draws' combiner (1-cycle; the player's draws use TEXEL0 or SHADE for colour, SHADE or TEXEL0 alpha)."""
    c1rgb, c1a, _, _ = g.decode_combine(*entry["combine"])
    prim = g.rgba_f(entry["prim"])
    env = g.rgba_f(entry["env"])
    tex = [x / 255.0 for x in tex] if tex else [0, 0, 0, 0]
    sh = [x / 255.0 for x in shade]
    src = {"TEXEL0": tex[:3], "SHADE": sh[:3], "PRIMITIVE": prim[:3], "ENVIRONMENT": env[:3], "0": [0] * 3, "1": [1] * 3,
           "TEXEL0_ALPHA": [tex[3]] * 3, "SHADE_ALPHA": [sh[3]] * 3, "PRIMITIVE_ALPHA": [prim[3]] * 3, "COMBINED": [0] * 3,
           "ENV_ALPHA": [env[3]] * 3, "TEXEL1": tex[:3], "PRIM_LOD_FRAC": [0.25] * 3, "K5": [0] * 3}
    asrc = {"TEXEL0": tex[3], "SHADE": sh[3], "PRIMITIVE": prim[3], "ENVIRONMENT": env[3], "0": 0, "1": 1,
            "COMBINED": 0, "TEXEL1": tex[3], "LOD_FRACTION": 0, "PRIM_LOD_FRAC": 0.25}
    a, b, c, d = (src.get(n, [0] * 3) for n in c1rgb)
    rgb = [(a[k] - b[k]) * c[k] + d[k] for k in range(3)]
    aa, ab, ac, ad = (asrc.get(n, 0) for n in c1a)
    al = (aa - ab) * ac + ad
    return [min(max(x, 0.0), 1.0) for x in rgb], min(max(al, 0.0), 1.0)


def translucent(entry):
    lo = entry["othermode_l"]
    return g.bits(lo, 10, 2) == 2 or (lo & g.RM_FORCE_BL and not lo & g.RM_Z_UPD)


def render(man, tris, project, size, uniform=None, bilinear=True, bg=(0, 0, 0, 0), dump_uniform=None, only=None,
           supersample=1):
    """Rasterise. tris: [(draw, [(xyz, s, t, rgba)] * 3)]; project(xyz) -> (x, y, depth) or None. Returns RGBA image.
    bilinear: True follows each draw's filter (as the game), False is nearest everywhere (to judge texels)."""
    if uniform is not None and uniform.tris is not None and tris is man.tris:
        tris = uniform.tris   # the set's override coordinates on the model
    W, H = size[0] * supersample, size[1] * supersample
    color = [bg] * (W * H)
    depth = [math.inf] * (W * H)
    samplers = {}
    order = sorted(range(len(tris)), key=lambda i: translucent(man.draws[tris[i][0]]))
    for ti in order:
        d, vs = tris[ti]
        e = man.draws[d]
        if only is not None and not (set(part_of(e)) & only):
            continue
        if d not in samplers:
            samplers[d] = Sampler(e, uniform, man.m["part_ids"], dump_uniform, d)
        smp = samplers[d]
        pts = [project(v[0]) for v in vs]
        if any(p is None for p in pts):
            continue
        pts = [(p[0] * supersample, p[1] * supersample, p[2]) for p in pts]
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = pts
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if area == 0:
            continue
        # gbi_dump.Renderer's rule: G_CULL_BACK drops screen-space area > 0 (y down), summed as x_a*y_b - x_b*y_a.
        wind = x0 * y1 - x1 * y0 + x1 * y2 - x2 * y1 + x2 * y0 - x0 * y2
        if e["cull_back"] and wind > 0:
            continue
        if e["cull_front"] and wind < 0:
            continue
        iw = [1.0 / p[2] for p in pts]
        st = [(v[1], v[2]) for v in vs]
        sh = [v[3] for v in vs]
        bil = bilinear and e["filter"] == "BILERP"
        trans = translucent(e)
        zupd = bool(e["othermode_l"] & g.RM_Z_UPD) or not trans
        minx, maxx = max(int(min(x0, x1, x2)), 0), min(int(max(x0, x1, x2)) + 1, W - 1)
        miny, maxy = max(int(min(y0, y1, y2)), 0), min(int(max(y0, y1, y2)) + 1, H - 1)
        inv_area = 1.0 / area
        for py in range(miny, maxy + 1):
            cy = py + 0.5
            for px in range(minx, maxx + 1):
                cx = px + 0.5
                b0 = ((x1 - cx) * (y2 - cy) - (x2 - cx) * (y1 - cy)) * inv_area
                b1 = ((x2 - cx) * (y0 - cy) - (x0 - cx) * (y2 - cy)) * inv_area
                b2 = 1.0 - b0 - b1
                if b0 < -1e-9 or b1 < -1e-9 or b2 < -1e-9:
                    continue
                q = b0 * iw[0] + b1 * iw[1] + b2 * iw[2]
                z = 1.0 / q
                k = py * W + px
                if z >= depth[k] - 1e-6 * z:
                    continue
                w0, w1, w2 = b0 * iw[0] * z, b1 * iw[1] * z, b2 * iw[2] * z
                shade = [sh[0][c] * w0 + sh[1][c] * w1 + sh[2][c] * w2 for c in range(4)]
                tex = None
                if smp.on:
                    s = st[0][0] * w0 + st[1][0] * w1 + st[2][0] * w2
                    t = st[0][1] * w0 + st[1][1] * w1 + st[2][1] * w2
                    tex = smp.sample(s, t, bil)
                rgb, al = combine(e, tex, shade)
                if trans:
                    if al <= 0.01:
                        continue
                    old = color[k]
                    rgb = [rgb[c] * al + old[c] / 255.0 * (1 - al) for c in range(3)]
                if zupd:
                    depth[k] = z
                color[k] = (int(rgb[0] * 255 + 0.5), int(rgb[1] * 255 + 0.5), int(rgb[2] * 255 + 0.5), 255)
    im = Image.new("RGBA", (W, H))
    im.putdata(color)
    if supersample > 1:
        im = im.resize(size, Image.BOX)
    return im


def camera(target, yaw_deg, dist, fov_deg, size, pitch_deg=0.0):
    """A camera orbiting the body frame's up axis: yaw 0 looks at the front, 90 at the player's left side."""
    yaw, pitch = math.radians(yaw_deg), math.radians(pitch_deg)
    d = [math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch)]  # target -> eye
    eye = [target[k] + d[k] * dist for k in range(3)]
    z = d
    x = [1.0 * z[2], 0.0, -z[0]]  # up x z with up = (0, 1, 0)
    n = math.hypot(x[0], x[2]) or 1.0
    x = [x[0] / n, 0.0, x[2] / n]
    y = [z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]]
    f = (size[1] / 2) / math.tan(math.radians(fov_deg) / 2)

    def project(p):
        r = [p[k] - eye[k] for k in range(3)]
        vx, vy, vz = (sum(r[k] * a[k] for k in range(3)) for a in (x, y, z))
        if vz > -1e-3:
            return None
        return (size[0] / 2 + f * vx / -vz, size[1] / 2 - f * vy / -vz, -vz)
    return project


def fit_camera(man, parts, yaw, size, fov=18.0, margin=1.08, pitch=0.0, dist_scale=1.0):
    """An orbit camera at yaw/pitch that frames the given parts' triangles (dist_scale > 1 backs off)."""
    pts = [v[0] for d, vs in man.tris if parts is None or set(man.draws[d].get("parts", [])) & set(parts) for v in vs]
    yw, pt = math.radians(yaw), math.radians(pitch)
    z = [math.sin(yw) * math.cos(pt), math.sin(pt), math.cos(yw) * math.cos(pt)]
    x = [z[2], 0.0, -z[0]]
    n = math.hypot(x[0], x[2]) or 1.0
    x = [x[0] / n, 0.0, x[2] / n]
    y = [z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]]
    px = [sum(p[k] * x[k] for k in range(3)) for p in pts]
    py = [sum(p[k] * y[k] for k in range(3)) for p in pts]
    pz = [sum(p[k] * z[k] for k in range(3)) for p in pts]
    cx, cy, cz = (min(px) + max(px)) / 2, (min(py) + max(py)) / 2, (min(pz) + max(pz)) / 2
    target = [cx * x[k] + cy * y[k] + cz * z[k] for k in range(3)]
    t = math.tan(math.radians(fov) / 2)
    half_w, half_h = (max(px) - min(px)) / 2, (max(py) - min(py)) / 2
    dist = max(half_h / t, half_w / (t * size[0] / size[1])) * margin + (max(pz) - min(pz)) / 2
    return camera(target, yaw, dist * dist_scale, fov, size, pitch)


def dump_camera(man, scale=4):
    """The dump's own camera (its projection and viewport), for comparing with a screenshot of the same frame."""
    proj = man.m["camera"]["proj"]
    (sx, sy, sz, _), (tx, ty, tz, _) = man.m["camera"]["viewport"]

    def project(p):
        x, y, z = p
        c = [x * proj[0][j] + y * proj[1][j] + z * proj[2][j] + proj[3][j] for j in range(4)]
        if c[3] <= 1e-6:
            return None
        return ((c[0] / c[3] * sx / 4 + tx / 4) * scale, (-c[1] / c[3] * sy / 4 + ty / 4) * scale, c[3])
    return project


def compare_image(man, u, shot, pad=6):
    """Side by side: a screenshot of the dump's frame (a `tools/live.sh shot` of a 4:3 window, title bar included),
    the set drawn by the previewer from the dump's camera, and the dump's own uniform drawn the same way."""
    s = Image.open(shot).convert("RGBA")
    title = s.height - s.width * 3 // 4                      # the window's title bar
    (vsx, vsy, _, _), _ = man.m["camera"]["viewport"]
    fb_w, fb_h = vsx / 2.0, vsy / 2.0                         # the frame the viewport covers (512x256 at team select)
    kx, ky = s.width / fb_w, (s.height - title) / fb_h       # the window shows it whole, stretched to 4:3
    box = man.m["source"]["screen_box"]
    x0, y0, x1, y1 = box[0] - pad, box[1] - pad, box[2] + pad, box[3] + pad
    crop = s.crop((int(x0 * kx), int(y0 * ky) + title, int(x1 * kx), int(y1 * ky) + title))
    f = min(1.0, 700.0 / crop.height)                        # the screenshot's pixels, at most 700 tall
    proj = dump_camera(man, 1)
    shift = lambda p: None if p is None else ((p[0] - x0) * kx * f, (p[1] - y0) * ky * f, p[2])  # noqa: E731
    size = (int((x1 - x0) * kx * f), int((y1 - y0) * ky * f))
    u.number = man.m.get("number", u.number)
    u._cache.clear()
    mine = render(man, man.view_tris, lambda p: shift(proj(p)), size, u, bg=PANEL, supersample=2)
    own = render(man, man.view_tris, lambda p: shift(proj(p)), size, dump_uniform=man.m.get("dump_uniform"), bg=PANEL,
                 supersample=2)
    crop = crop.resize(size, Image.LANCZOS)
    return row(f"{u.label}: previewer vs game", [labelled(crop, "Game", Path(shot).name),
                                                 labelled(mine, "Previewer", "this set, the dump's camera"),
                                                 labelled(own, "Dump's own textures", "the same frame's RDRAM")])


def check_image(path, out, scale=4):
    """The mannequin from the dump's camera with the dump's own uniform (make_mannequin.py's check)."""
    man = Mannequin(path)
    box = man.m["source"]["screen_box"]
    proj = dump_camera(man, scale)
    pad = 6
    shift = lambda p: None if p is None else (p[0] - (box[0] - pad) * scale, p[1] - (box[1] - pad) * scale, p[2])  # noqa: E731
    size = (int((box[2] - box[0] + 2 * pad) * scale), int((box[3] - box[1] + 2 * pad) * scale))
    im = render(man, man.view_tris, lambda p: shift(proj(p)), size, dump_uniform=man.m.get("dump_uniform"), bg=BG)
    im.save(out)
    return im


# ---------------------------------------------------------------------------------------------------------------
# The contact sheet
# ---------------------------------------------------------------------------------------------------------------

def font(size):
    for f in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"):
        if Path(f).exists():
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def labelled(im, text, sub=None, pad=8):
    f, fs = font(15), font(12)
    h = 22 + (16 if sub else 0)
    out = Image.new("RGBA", (max(im.width, 120) + 2 * pad, im.height + h + 2 * pad), PANEL)
    d = ImageDraw.Draw(out)
    d.text((pad, pad - 2), text, font=f, fill=INK)
    if sub:
        d.text((pad, pad + 17), sub, font=fs, fill=DIM)
    out.alpha_composite(im, (pad, pad + h))
    return out


def row(title, tiles, note=None):
    f = font(20)
    gap = 10
    h = max(t.height for t in tiles) + 40
    w = sum(t.width for t in tiles) + gap * (len(tiles) + 1)
    out = Image.new("RGBA", (max(w, 400), h), BG)
    d = ImageDraw.Draw(out)
    d.text((gap, 6), title, font=f, fill=INK)
    if note:
        d.text((gap + d.textlength(title, font=f) + 16, 11), note, font=font(13), fill=DIM)
    x = gap
    for t in tiles:
        out.alpha_composite(t, (x, 34))
        x += t.width + gap
    return out


def stack(rows):
    W = max(r.width for r in rows)
    out = Image.new("RGBA", (W, sum(r.height for r in rows)), BG)
    y = 0
    for r in rows:
        out.alpha_composite(r, (0, y))
        y += r.height
    return out


HELMET = {"helmet_side", "helmet_stripe_front", "helmet_stripe_back", "facemask"}
GAME_HELMET_PX = 30   # the helmet's height on screen in a gameplay close-up (huddle camera at 1280x960, measured)


def player_rows(man, u, number, full=True):
    u.number = number
    u._cache.clear()
    body = (260, 400)
    views = [("Front", 0), ("Three-quarter", 35), ("Left side", 90), ("Back", 180), ("Right side", -90)]
    tiles = [labelled(render(man, man.tris, fit_camera(man, None, yaw, body, pitch=4), body, u, bg=PANEL,
                             supersample=2), name) for name, yaw in views]
    rows = [row(f"{u.label}: player", tiles, f"#{number}, game filtering" + (f"; {'; '.join(u.notes)}" if u.notes else ""))]
    if not full:
        return rows
    # Helmets: game size (the game's own filtering), then 4x with nearest-neighbour texels.
    hs = []
    for name, yaw in (("Helmet, left side", 90), ("Helmet, right side", -90)):
        small = render(man, man.tris, fit_camera(man, HELMET, yaw, (GAME_HELMET_PX + 6,) * 2, fov=20, margin=1.0),
                       (GAME_HELMET_PX + 6,) * 2, u, bg=PANEL, supersample=4)
        big = render(man, man.tris, fit_camera(man, HELMET, yaw, (256, 256), fov=20, margin=1.0), (256, 256), u,
                     bilinear=False, bg=PANEL)
        hs.append(labelled(small, name, "game size"))
        hs.append(labelled(big, name, "4x, nearest"))
    rows.append(row("Helmet", hs, "the left half of helmet_side wraps one side, the right half the other"))
    # Numbers as worn (the torso close up, back and front), the jersey texture with them, the numbers strip.
    torso = {"jersey", "shoulder"}
    ns = [labelled(render(man, man.tris, fit_camera(man, torso, yaw, (220, 220), margin=0.95), (220, 220), u,
                          bg=PANEL, supersample=2), name) for name, yaw in (("Back", 180), ("Front", 0))]
    ns.append(labelled(u.jersey_image().resize((160, 160), Image.NEAREST), "jersey + numbers", "as built in game, 4x"))
    strip = u.parts["numbers"].image()
    ns.append(labelled(strip.resize((strip.width * 3, strip.height * 3), Image.NEAREST), "numbers part", "3x"))
    rows.append(row("Numbers", ns))
    # Pants from the side.
    legs = {"pants", "pant_stripe", "socks"}
    ps = [labelled(render(man, man.tris, fit_camera(man, legs, yaw, (220, 260), margin=0.95), (220, 260), u,
                          bg=PANEL, supersample=2), name) for name, yaw in (("Left side", 90), ("Right side", -90))]
    rows.append(row("Pants", ps))
    # The converted textures, as the game stores them.
    ts = []
    for p in PARTS:
        im = u.parts[p].image()
        k = 4 if im.width <= 40 else 2
        ts.append(labelled(im.resize((im.width * k, im.height * k), Image.NEAREST), p, f"{im.width}x{im.height}"))
    rows.append(row("Textures", ts, "as imported (helmet side in the game's stored layout)"))
    return rows


HTML = """<!doctype html><html><head><meta charset="utf-8"><title>Uniform preview</title>
<style>body{margin:0;background:#2c2e3a;color:#eee;font:14px -apple-system,Helvetica,sans-serif;overflow:hidden}
#bar{position:fixed;top:0;left:0;right:0;padding:8px 12px;background:#0008;display:flex;gap:12px;align-items:center}
canvas{display:block;width:100vw;height:100vh}select,button{font:inherit}</style></head><body>
<div id="bar"><b>Uniform preview</b><select id="set"></select><label><input type="checkbox" id="near"> nearest texels</label>
<button data-v="0,0">Front</button><button data-v="35,0">3/4</button><button data-v="90,0">Left</button>
<button data-v="180,0">Back</button><button data-v="-90,0">Right</button><button data-v="h90">Helmet L</button>
<button data-v="h-90">Helmet R</button><span style="opacity:.7">drag to orbit, wheel to zoom</span></div>
<canvas id="c"></canvas><script>
const D=__DATA__;
const cv=document.getElementById('c'),gl=cv.getContext('webgl',{antialias:true});
const vs=`attribute vec3 p;attribute vec2 uv;attribute vec4 col;uniform mat4 m;varying vec2 v;varying vec4 c;
void main(){gl_Position=m*vec4(p,1.);v=uv;c=col;}`;
const fs=`precision mediump float;varying vec2 v;varying vec4 c;uniform sampler2D t;uniform int mode;
void main(){vec4 x=texture2D(t,v);if(mode==0)gl_FragColor=vec4(c.rgb,1.);else if(mode==1)gl_FragColor=vec4(x.rgb,1.);
else{if(x.a<0.02)discard;gl_FragColor=x;}}`;
function sh(k,s){const o=gl.createShader(k);gl.shaderSource(o,s);gl.compileShader(o);return o}
const pr=gl.createProgram();gl.attachShader(pr,sh(gl.VERTEX_SHADER,vs));gl.attachShader(pr,sh(gl.FRAGMENT_SHADER,fs));
gl.linkProgram(pr);gl.useProgram(pr);
const L={p:gl.getAttribLocation(pr,'p'),uv:gl.getAttribLocation(pr,'uv'),col:gl.getAttribLocation(pr,'col'),
m:gl.getUniformLocation(pr,'m'),mode:gl.getUniformLocation(pr,'mode')};
const draws=D.draws.map(d=>{const b=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,b);
gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(d.v),gl.STATIC_DRAW);return Object.assign({buf:b,n:d.v.length/9},d)});
let tex=[];const sel=document.getElementById('set');
D.sets.forEach((s,i)=>{const o=document.createElement('option');o.value=i;o.textContent=s.label;sel.appendChild(o)});
function wrapOf(cm,mask){return (cm&2)||!mask?gl.CLAMP_TO_EDGE:(cm&1?gl.MIRRORED_REPEAT:gl.REPEAT)}
function load(i){tex=D.sets[i].tex.map((src,k)=>{if(!src)return null;const t=gl.createTexture();const im=new Image();
im.onload=()=>{gl.bindTexture(gl.TEXTURE_2D,t);gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,im);setp();draw()};
im.src=src;t.d=draws[k];return t});}
function setp(){const near=document.getElementById('near').checked;tex.forEach(t=>{if(!t)return;const d=t.d;
gl.bindTexture(gl.TEXTURE_2D,t);const f=(near||d.filter!='BILERP')?gl.NEAREST:gl.LINEAR;
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,f);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,f);
gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,wrapOf(d.cms,d.masks));gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,wrapOf(d.cmt,d.maskt));})}
let yaw=20,pitch=5,dist=1,tgt=D.body;
function mat(){const a=innerWidth/innerHeight,f=1/Math.tan(9*Math.PI/180),y=yaw*Math.PI/180,p=pitch*Math.PI/180;
const z=[Math.sin(y)*Math.cos(p),Math.sin(p),Math.cos(y)*Math.cos(p)];let x=[z[2],0,-z[0]];const n=Math.hypot(x[0],x[2]);x=[x[0]/n,0,x[2]/n];
const u=[z[1]*x[2]-z[2]*x[1],z[2]*x[0]-z[0]*x[2],z[0]*x[1]-z[1]*x[0]];const e=tgt.c.map((c,k)=>c+z[k]*tgt.r*dist/Math.tan(9*Math.PI/180)*1.1);
const dot=(a,b)=>a[0]*b[0]+a[1]*b[1]+a[2]*b[2];const nr=tgt.r*0.05,fr=tgt.r*40;
const V=[x[0],u[0],z[0],0,x[1],u[1],z[1],0,x[2],u[2],z[2],0,-dot(x,e),-dot(u,e),-dot(z,e),1];
const P=[f/a,0,0,0,0,f,0,0,0,0,(fr+nr)/(nr-fr),-1,0,0,2*fr*nr/(nr-fr),0];const M=new Array(16).fill(0);
for(let i=0;i<4;i++)for(let j=0;j<4;j++)for(let k=0;k<4;k++)M[i*4+j]+=V[i*4+k]*P[k*4+j];return M}
function draw(){cv.width=innerWidth*devicePixelRatio;cv.height=innerHeight*devicePixelRatio;gl.viewport(0,0,cv.width,cv.height);
gl.clearColor(.25,.27,.33,1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);gl.enable(gl.DEPTH_TEST);gl.frontFace(gl.CCW);
gl.uniformMatrix4fv(L.m,false,new Float32Array(mat()));
const order=draws.map((d,i)=>i).sort((a,b)=>draws[a].trans-draws[b].trans);
for(const i of order){const d=draws[i];if(d.cull){gl.enable(gl.CULL_FACE);gl.cullFace(d.cull)}else gl.disable(gl.CULL_FACE);
if(d.trans){gl.enable(gl.BLEND);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);gl.depthMask(false)}else{gl.disable(gl.BLEND);gl.depthMask(true)}
gl.bindBuffer(gl.ARRAY_BUFFER,d.buf);gl.enableVertexAttribArray(L.p);gl.vertexAttribPointer(L.p,3,gl.FLOAT,false,36,0);
gl.enableVertexAttribArray(L.uv);gl.vertexAttribPointer(L.uv,2,gl.FLOAT,false,36,12);gl.enableVertexAttribArray(L.col);gl.vertexAttribPointer(L.col,4,gl.FLOAT,false,36,20);
const t=tex[i];gl.uniform1i(L.mode,!t?0:(d.trans?2:1));if(t)gl.bindTexture(gl.TEXTURE_2D,t);gl.drawArrays(gl.TRIANGLES,0,d.n)}}
let drag=null;cv.onmousedown=e=>drag=[e.clientX,e.clientY];onmouseup=()=>drag=null;
onmousemove=e=>{if(!drag)return;yaw-=(e.clientX-drag[0])*0.5;pitch=Math.max(-80,Math.min(80,pitch+(e.clientY-drag[1])*0.5));drag=[e.clientX,e.clientY];draw()};
cv.onwheel=e=>{dist*=Math.exp(e.deltaY*0.001);draw();e.preventDefault()};onresize=draw;
document.querySelectorAll('button').forEach(b=>b.onclick=()=>{const v=b.dataset.v;if(v[0]=='h'){tgt=D.helmet;yaw=+v.slice(1);pitch=0;dist=0.9}
else{tgt=D.body;[yaw,pitch]=v.split(',').map(Number);dist=1}draw()});
sel.onchange=()=>load(sel.value);document.getElementById('near').onchange=()=>{setp();draw()};load(0);draw();
</script></body></html>"""


def write_html(man, sets, out, number):
    """A self-contained page: the mannequin in WebGL with each set's textures (as the previewer samples them, one
    small texture per draw with the draw's wrap, mirror, clamp and filter)."""
    import base64
    import io
    draws = []
    for di, e in enumerate(man.draws):
        v = []
        tl = e.get("tile")
        w, h = e.get("size", [1, 1])
        for d, vs in man.tris:
            if d != di:
                continue
            for p, s, t, col in vs:
                if tl:
                    s = (Sampler._shift(s, tl["shifts"]) - tl["uls"] / 4.0) / w
                    t = (Sampler._shift(t, tl["shiftt"]) - tl["ult"] / 4.0) / h
                v += [*p, s, t, *(c / 255.0 for c in col)]
        cull = 0x0405 if e["cull_back"] else (0x0404 if e["cull_front"] else 0)  # GL_BACK / GL_FRONT, CCW front
        draws.append({"v": [round(x, 4) for x in v], "cull": cull, "trans": 1 if translucent(e) else 0,
                      "filter": e["filter"], **({k: tl[k] for k in ("cms", "cmt", "masks", "maskt")} if tl else {})})
    data_sets = []
    for u in sets:
        u.number = number
        u._cache.clear()
        texs = []
        for d, e in enumerate(man.draws):
            if not e.get("tex_on"):
                texs.append(None)
                continue
            smp = Sampler(e, u, man.m["part_ids"], None, d)
            im = Image.new("RGBA", (smp.w, smp.h))
            im.putdata([tuple(int(c) for c in px) for px in smp.tex])
            buf = io.BytesIO()
            im.save(buf, "PNG")
            texs.append("data:image/png;base64," + base64.b64encode(buf.getvalue()).decode())
        data_sets.append({"label": u.label, "tex": texs})
    body_c, body_r = man.centre(PARTS)
    hel_c, hel_r = man.centre(HELMET)
    data = {"draws": draws, "sets": data_sets, "body": {"c": body_c, "r": body_r}, "helmet": {"c": hel_c, "r": hel_r}}
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text(HTML.replace("__DATA__", json.dumps(data, separators=(",", ":"))))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("folder", type=Path, nargs="?", help="a set or team folder (or name the parts one by one below)")
    ap.add_argument("--out", type=Path, help="sheet PNG (default build/uniform_preview/<team>_<set>.png); several sets: "
                    "one sheet each, named <out>_<set>.png")
    ap.add_argument("--number", type=int, default=18, help="jersey number to draw (default 18)")
    ap.add_argument("--team", "--base", dest="team", help="the NFL team whose layout the set is on (e.g. LAR), when the "
                    "folder's name doesn't say; needed with the per-part options")
    for p in PARTS:
        ap.add_argument(f"--{p.replace('_', '-')}", type=Path, metavar="PNG", help=f"{p}.png (parts left out: the team's away part)")
    ap.add_argument("--set", help="only the set whose name or folder contains this")
    ap.add_argument("--reference", action="store_true", help="add a row with the NFL team the set is based on")
    ap.add_argument("--app-folder", type=Path, default=ROOT / "build" / "appdata2026",
                    help="for --reference: where the NFL teams' installed sets are (default build/appdata2026)")
    ap.add_argument("--mannequin", type=Path, default=MANNEQUIN)
    ap.add_argument("--yaw", type=float, help="instead of the sheet, one view: degrees around the player (0 front, "
                    "90 his left side, 180 back, -90 his right side)")
    ap.add_argument("--pitch", type=float, default=0.0, help="with --yaw: degrees above (+) or below (-) the player")
    ap.add_argument("--dist", type=float, default=1.0, help="with --yaw: distance, 1 frames the target, 0.5 twice as close")
    ap.add_argument("--target", choices=["player", "helmet", "torso", "legs"], default="player", help="with --yaw: what to frame")
    ap.add_argument("--size", default="600x800", help="with --yaw: image size WxH (default 600x800)")
    ap.add_argument("--nearest", action="store_true", help="with --yaw: nearest-neighbour texels instead of the game's filtering")
    ap.add_argument("--compare", type=Path, metavar="SHOT", help="side by side with a screenshot of the mannequin's own frame "
                    "(tools/live.sh shot taken with the dump): the match-vs-game check")
    ap.add_argument("--html", type=Path, help="also write an interactive page (drag to orbit, wheel to zoom) for the sets")
    a = ap.parse_args(argv)
    man = Mannequin(a.mannequin)
    scratch = Path(tempfile.mkdtemp(prefix="uniform_preview_"))
    try:
        given = {p: getattr(a, p) for p in PARTS if getattr(a, p)}
        if given:
            if not a.team:
                raise SystemExit("with the per-part options, say which NFL team's layout: --base LAR")
            d = scratch / "given" / a.team.upper() / "set"
            d.mkdir(parents=True)
            for p, png in given.items():
                (d / f"{p}.png").symlink_to(png.resolve())
            a.folder = a.folder or d
            sets, base = load(d, scratch, a.team, man)
        elif a.folder:
            sets, base = load(a.folder, scratch, a.team, man)
        else:
            raise SystemExit("give a folder, or the parts one by one (--jersey x.png ... --base LAR)")
        if a.set:
            sets = [u for u in sets if a.set.lower() in u.label.lower()]
        ref = reference_uniform(base, a.app_folder if a.app_folder.exists() else None) if a.reference and base else None
        outs = []
        if a.html:
            write_html(man, sets, a.html, a.number)
            print(f"{a.html}: interactive view")
        for u in sets:
            u.number = a.number
            if a.yaw is not None:
                w, h = (int(x) for x in a.size.split("x"))
                target = {"player": None, "helmet": HELMET, "torso": {"jersey", "shoulder"},
                          "legs": {"pants", "pant_stripe", "socks"}}[a.target]
                sheet = render(man, man.tris, fit_camera(man, target, a.yaw, (w, h), pitch=a.pitch, dist_scale=a.dist),
                               (w, h), u, bilinear=not a.nearest, bg=PANEL, supersample=1 if a.nearest else 2)
                rows = None
            elif a.compare:
                sheet = compare_image(man, u, a.compare)
                rows = None
            else:
                rows = player_rows(man, u, a.number)
            if rows is None:
                pass
            elif ref:
                rows += player_rows(man, ref, a.number, full=False)
            if rows is not None:
                sheet = stack(rows)
            name = f"{a.folder.name}_{u.label}".replace(" ", "_").replace("/", "_")
            if a.out:
                out = a.out if len(sets) == 1 else a.out.with_name(f"{a.out.stem}_{u.label.replace(' ', '_')}{a.out.suffix}")
            else:
                out = ROOT / "build" / "uniform_preview" / f"{name}.png"
            out.parent.mkdir(parents=True, exist_ok=True)
            sheet.convert("RGB").save(out)
            outs.append(out)
            print(f"{out}: {u.label}" + (f" ({'; '.join(u.notes)})" if u.notes else ""))
        return outs
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
