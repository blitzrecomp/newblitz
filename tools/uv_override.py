#!/usr/bin/env python3
"""Per-set texture-coordinate overrides for a league pak (src/game/uv_overrides.cpp): a uniform part drawn with its
own texture AND its own texture coordinates on the player models, for the sets a design table names.

  .venv/bin/python tools/uv_override.py --table <dir>/shoulder_uv.json --pack <app folder>/packs/<id> [--name shoulder_caps]
                                        [--rom build/baserom.z64] [--mannequin build/uniform_mannequin/mannequin.json]

The table (the design handoff's, e.g. Pac-12 stripe_review/dev_candidate/shoulder_uv.json) gives, for one part's draws on
the sampled team-select model (tools/make_mannequin.py), each triangle corner's original and proposed texel coordinates,
the mannequin's SHA-256 and the sets it is for ({"CSU": ["home", "away", "ag_day"], ...}). Next to it,
<TEAM>/<part>_<home|away>.png and <TEAM>/alternates/<set>/<part>.png are the matching textures.

What this does, offline, from your ROM (the player models are main's static data, the same in every game):
  * checks the mannequin's SHA-256 (when it is here) and every original coordinate against the model's own vertices;
  * gives each vertex of the sampled draws its proposed coordinate (a vertex shared by several corners must get one);
  * finds the same part's draws on every player model (main's model table: 2 builds x 3 levels of detail) and carries
    the proposed coordinates over by vertex position (each cap's box normalised); a model whose cap doesn't match
    closely (the low-detail ones) is left out, so it keeps the stock texture with its stock coordinates;
  * writes each set's texture into files/added (<id>_shu_<h|a|k>.wms, CI4 with its exact palette) and pack.json's
    "uv_overrides" (the table, per model block: the vertices' original and new raw S/T) and each set's "uv_override".

  .venv/bin/python tools/uv_override.py --sleeves <handoff>/component_audit/right_arm_assets.json --pack <pack folder>

--sleeves binds per-cap textures instead (the CFL's right sleeves): the part keeps its stock coordinates and the set's
own texture, and the cap a companion names (the player's right arm) samples the companion. The table it writes lists
every model's shoulder block and each cap's bone matrix ("caps"); make_league_pak.py --handoff --sleeves runs it.

The app applies a set's table only to the players wearing that set (the shoulder part file their side was dressed in),
and only on the blocks listed; everything else keeps the stock texture paired with the stock coordinates.
"""
import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

MAIN_ROM, MAIN_VRAM = 0x2FC20, 0x80248F00   # v1.1: main's code and data (build/syms/blitz2000.syms.toml), uncompressed
MODEL_TABLE, MODELS = 0x8029E130, 6          # main: the player model descriptors (body DL at +8)
MODEL_NAMES = ["normal high", "normal mid", "normal low", "big high", "big mid", "big low"]
# A uniform part's tile in the body draws and its buffer in a side's uniform (main func_80256580; mixed_uniforms.cpp).
# "caps": the part's draws in its block, in order, by the arm they cover (the player's own right and left). Measured on
# the team-select model with a texel-coloured texture (the CFL handoff's component_audit/SHOULDER_BINDING.md: right arm
# draw 9, left arm draw 10); every model's block places its draws with the same bone matrices in that order.
PARTS = {"shoulder": {"tile": 2, "code": "shu", "base_code": "shl", "caps": ["right", "left"],
                      "cap_code": {"right": "shr", "left": "shf"}}}


class Rom:
    def __init__(self, path):
        self.b = Path(path).read_bytes()

    def at(self, va, n):
        o = MAIN_ROM + (va - MAIN_VRAM)
        return self.b[o:o + n]

    def w(self, va):
        return struct.unpack(">II", self.at(va, 8))

    def u32(self, va):
        return struct.unpack(">I", self.at(va, 4))[0]

    def vtx(self, va, n):
        out = []
        for i in range(n):
            x, y, z, _f, s, t = struct.unpack(">hhhHhh", self.at(va + 16 * i, 12))
            out.append((x, y, z, s, t))
        return out


def tri_cmds(w0, w1):
    op = w0 >> 24
    if op == 0x05:
        return [tuple(((w0 >> s) & 0xFF) // 2 for s in (16, 8, 0))]
    if op == 0x06:
        return [tuple(((w0 >> s) & 0xFF) // 2 for s in (16, 8, 0)), tuple(((w1 >> s) & 0xFF) // 2 for s in (16, 8, 0))]
    return None


def blocks_of(rom, body_dl, tile):
    """The part's blocks in a body display list: from its G_TEXTURE (tile) to the next G_TEXTURE / G_ENDDL, with each
    draw's G_MTX, G_VTX (address, count) and triangles."""
    out, cur, a = [], None, body_dl
    for _ in range(400):
        w0, w1 = rom.w(a)
        op = w0 >> 24
        if op in (0xD7, 0xDF):
            if cur:
                cur["end"] = a
                out.append(cur)
                cur = None
            if op == 0xDF:
                break
            if (w0 >> 8) & 7 == tile and w0 & 0xFF:
                cur = {"block": a, "scale": (w1 >> 16, w1 & 0xFFFF), "draws": [], "cmds": 1}
        elif cur is not None:
            cur["cmds"] += 1
            if op == 0xDA:
                cur["draws"].append({"mtx": w1, "tris": []})
            elif op == 0x01:
                n = (w0 >> 12) & 0xFF
                v0 = ((w0 >> 1) & 0x7F) - n
                if v0 != 0 or not cur["draws"]:
                    raise SystemExit(f"{a:08X}: a vertex load this tool doesn't follow (v0 {v0})")
                cur["draws"][-1].update(vtx=w1, n=n, first=a + 8)
            elif tri_cmds(w0, w1):
                cur["draws"][-1]["tris"] += tri_cmds(w0, w1)
            else:
                # Anything else (a state change, a call) ends the block: the app jumps back here.
                cur["cmds"] -= 1
                cur["end"] = a
                out.append(cur)
                cur = None
        a += 8
    return out


def norm(vs):
    lo = [min(v[i] for v in vs) for i in range(3)]
    hi = [max(v[i] for v in vs) for i in range(3)]
    return [tuple((v[i] - lo[i]) / max(1, hi[i] - lo[i]) for i in range(3)) for v in vs]


def sleeves(assets_path, pack_dir, name="shoulder_sleeves", part_name="shoulder", rom_path=None):
    """Per-cap textures with the stock coordinates: a design list of companion textures (e.g. the CFL's
    component_audit/right_arm_assets.json: per asset "team", "set", "optional_<cap>" (a PNG, relative to
    "optional_paths_relative_to") and "<cap>_sha256") -> files/added/<id>_<cap code>_<h|a|n>.wms, a "uv_overrides"
    table with every model's part block at its stock coordinates and its "caps" (each cap's bone matrix), and each
    set's "uv_override": {"table", "caps": {"<cap>": file}}. The rest of the part keeps the set's own texture."""
    part = PARTS[part_name]
    rom = Rom(rom_path or ROOT / "build" / "baserom.z64")
    assets_path = Path(assets_path)
    doc = json.loads(assets_path.read_text())
    base = (assets_path.parent / doc.get("optional_paths_relative_to", ".")).resolve()
    report = []
    # Every model's block, stock coordinates; its draws' bone matrices must be the caps' on every model.
    descs = [rom.u32(MODEL_TABLE + 4 * k) for k in range(MODELS)]
    meshes, mtx = [], None
    for m, d in enumerate(descs):
        blocks = blocks_of(rom, rom.u32(d + 8), part["tile"])
        if len(blocks) != 1:
            report.append(f"model {m} ({MODEL_NAMES[m]}): {len(blocks)} {part_name} blocks: left stock")
            continue
        blk = blocks[0]
        got = [dr["mtx"] for dr in blk["draws"]]
        if len(got) != len(part["caps"]) or (mtx and got != mtx):
            report.append(f"model {m} ({MODEL_NAMES[m]}): draws on bones {[f'{x:08X}' for x in got]}: left stock")
            continue
        mtx = got
        verts = []
        for dr in blk["draws"]:
            st = [[v[3], v[4]] for v in rom.vtx(dr["vtx"], dr["n"])]
            verts.append({"at": f"{dr['vtx']:08X}", "st": st, "uv": st})
        meshes.append({"model": m, "block": f"{blk['block']:08X}", "end": f"{blk['end']:08X}", "vertices": verts})
        report.append(f"model {m} ({MODEL_NAMES[m]}): block {blk['block']:08X}, caps " +
                      ", ".join(f"{c} {x:08X}" for c, x in zip(part["caps"], got)))
    if not meshes:
        raise SystemExit(f"no model's {part_name} block has its caps")
    caps = dict(zip(part["caps"], mtx))

    pack_path = pack_dir / "pack.json"
    pack = json.loads(pack_path.read_text())
    sys.path.insert(0, str(ROOT / "tools" / "announcer"))
    import make_league_pak as mlp
    added = pack_dir / "files" / "added"
    written = []
    for asset in doc["assets"]:
        tid, set_id = asset["team"], asset["set"]
        team = next((t for t in pack["teams"] if t["id"] == tid), None)
        if team is None:
            raise SystemExit(f"{tid}: not a team of {pack_path}")
        if set_id in ("home", "away"):
            stock = team.get("uniform_files", {}).get(f"{part_name}_{set_id}")
            holder = team.setdefault("uv_override", {})
            key = set_id
        else:
            us = next((u for u in team.get("uniform_sets", []) if u["id"] == set_id), None)
            if us is None:
                raise SystemExit(f"{tid}: no uniform set {set_id} in {pack_path}")
            stock = us.get("files", {}).get(part_name)
            holder, key = us, "uv_override"
        if not stock:
            raise SystemExit(f"{tid} {set_id}: the set has no {part_name} file of its own to key the override on")
        d = (added / stock).read_bytes()
        typ, w, h, ox, oy = struct.unpack_from(">IIIii", d, 4)
        ref = {"table": name, "caps": {}}
        for cap in part["caps"]:
            rel = asset.get(f"optional_{cap}")
            if not rel:
                continue
            png = base / rel
            want = asset.get(f"{cap}_sha256")
            got = hashlib.sha256(png.read_bytes()).hexdigest()
            if want and got != want:
                raise SystemExit(f"{png}: SHA-256 {got}, the list says {want}")
            img = Image.open(png).convert("RGBA")
            if img.size != (w, h) or typ != 16:
                raise SystemExit(f"{png}: {img.size} (type {typ}); the set's {part_name} is {(w, h)}, CI4")
            out = stock.replace(f"_{part['base_code']}_", f"_{part['cap_code'][cap]}_")
            if out == stock:
                raise SystemExit(f"{stock}: can't name its {cap} cap texture")
            (added / out).write_bytes(mlp.encode_ci4_exact(img, d[3], ox, oy))
            ref["caps"][cap] = out
            written.append(f"{tid} {set_id}: {stock} + {cap} cap {out} ({png.name}, SHA-256 {got[:16]}... checked)")
        if not ref["caps"]:
            continue
        if key == "uv_override":
            holder["uv_override"] = ref
        else:
            holder[key] = ref
    pack["uv_overrides"] = {**pack.get("uv_overrides", {}), name: {
        "part": part_name,
        "source": {"assets_sha256": hashlib.sha256(assets_path.read_bytes()).hexdigest()},
        "caps": {c: f"{x:08X}" for c, x in caps.items()},
        "meshes": meshes,
    }}
    pack_path.write_text(json.dumps(pack, indent=1) + "\n")
    print("\n".join(report))
    print("\n".join(written))
    print(f"{pack_path}: \"uv_overrides\".{name}: {len(meshes)} model blocks at stock coordinates, {len(written)} cap textures")
    return written


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--table", type=Path, help="a design table of new coordinates (with its textures beside it)")
    ap.add_argument("--sleeves", type=Path, help="instead: a list of per-cap companion textures at the stock coordinates "
                    "(the CFL's component_audit/right_arm_assets.json)")
    ap.add_argument("--pack", required=True, type=Path, help="the pak's pack folder (pack.json, files/added)")
    ap.add_argument("--name", help="the table's name in pack.json (default shoulder_caps; shoulder_sleeves with --sleeves)")
    ap.add_argument("--part", default="shoulder", choices=sorted(PARTS))
    ap.add_argument("--rom", default=str(ROOT / "build" / "baserom.z64"), help="the v1.1 ROM Modern runs on")
    ap.add_argument("--mannequin", default=str(ROOT / "build" / "uniform_mannequin" / "mannequin.json"))
    a = ap.parse_args(argv)
    if bool(a.table) == bool(a.sleeves):
        ap.error("give --table or --sleeves")
    if a.sleeves:
        sleeves(a.sleeves, a.pack, a.name or "shoulder_sleeves", a.part, a.rom)
        return
    a.name = a.name or "shoulder_caps"
    part = PARTS[a.part]
    table = json.loads(a.table.read_text())
    rom = Rom(a.rom)
    report = []

    # 1. The fingerprint: the mannequin the table was made on, and its original coordinates against the ROM's model.
    man = Path(a.mannequin)
    if man.exists():
        got = hashlib.sha256(man.read_bytes()).hexdigest()
        if got != table["mannequin_sha256"]:
            raise SystemExit(f"{man}: SHA-256 {got}, the table's mannequin is {table['mannequin_sha256']}")
        mj = json.loads(man.read_text())
        report.append(f"mannequin {man.name}: SHA-256 matches ({got[:16]}...)")
    else:
        mj = None
        report.append(f"mannequin not here ({man}): fingerprint checked against the ROM's model only")
    by_draw = {}
    for t in table["triangles"]:
        by_draw.setdefault((t["draw"], int(t["display_list"], 16)), []).append(t)
    sampled = {}   # first triangle command address -> {vertex index: (u, v)}
    for (draw, dl), tris in sorted(by_draw.items()):
        if mj:
            if mj["draws"][draw]["dl"] != f"{dl:08X}" or a.part not in mj["draws"][draw].get("parts", []):
                raise SystemExit(f"table draw {draw}: the mannequin's draw {draw} is {mj['draws'][draw]['dl']} "
                                 f"({mj['draws'][draw].get('parts')})")
            mtris = [x for x in mj["tris"] if x[0] == draw]
            for t in tris:
                m = mj["tris"][t["triangle"]]
                if m[0] != draw or any(abs(m[1 + c][3] - t["original_uv"][c][0]) > 1e-6 or abs(m[1 + c][4] - t["original_uv"][c][1]) > 1e-6
                                       for c in range(3)):
                    raise SystemExit(f"table triangle {t['triangle']}: original coordinates differ from the mannequin's")
            if len(mtris) != len(tris):
                raise SystemExit(f"draw {draw}: {len(mtris)} triangles in the mannequin, {len(tris)} in the table")
        sampled[dl] = tris
    # The sampled model's blocks (descriptor 0), matched to the table's draws by their first triangle command.
    descs = [rom.u32(MODEL_TABLE + 4 * k) for k in range(MODELS)]
    bodies = [rom.u32(d + 8) for d in descs]
    model_blocks = [blocks_of(rom, b, part["tile"]) for b in bodies]
    ref_vertices = {}   # draw index in the block -> [(x, y, z, u, v)] of the sampled model
    ref_block = None
    for blk in model_blocks[0]:
        firsts = [d["first"] for d in blk["draws"]]   # each draw's first triangle command (the mannequin's "dl")
        if all(f in sampled for f in firsts) and len(firsts) == len(sampled):
            ref_block = blk
    if ref_block is None:
        raise SystemExit(f"the table's draws {[f'{d:08X}' for d in sampled]} aren't the {a.part} draws of the sampled model")
    ss, ts = ref_block["scale"]
    to_raw = (32 * 65536 / ss, 32 * 65536 / ts)
    for k, d in enumerate(ref_block["draws"]):
        tris = sampled[d["first"]]
        if len(tris) != len(d["tris"]):
            raise SystemExit(f"draw at {d['first']:08X}: {len(d['tris'])} triangles in the ROM, {len(tris)} in the table")
        vs = rom.vtx(d["vtx"], d["n"])
        uv = {}
        for t, idx in zip(tris, d["tris"]):
            for c in range(3):
                v = vs[idx[c]]
                o = t["original_uv"][c]
                if abs(v[3] / to_raw[0] - o[0]) > 1e-6 or abs(v[4] / to_raw[1] - o[1]) > 1e-6:
                    raise SystemExit(f"triangle {t['triangle']} corner {c}: original {o}, the ROM's vertex has "
                                     f"{(v[3] / to_raw[0], v[4] / to_raw[1])}")
                p = tuple(t["proposed_uv_texels"][c])
                if idx[c] in uv and max(abs(x - y) for x, y in zip(uv[idx[c]], p)) > 1e-3:
                    raise SystemExit(f"vertex {idx[c]} of the draw at {d['first']:08X} gets two proposed coordinates "
                                     f"({uv[idx[c]]}, {p}): a seam this tool doesn't split")
                uv[idx[c]] = p
        if len(uv) != d["n"]:
            raise SystemExit(f"draw at {d['first']:08X}: {d['n'] - len(uv)} vertices without a proposed coordinate")
        ref_vertices[k] = [(*vs[i][:3], *uv[i]) for i in range(d["n"])]
    report.append(f"original coordinates: all {len(table['triangles'])} triangles match the ROM's model "
                  f"({', '.join(f'{d:08X}' for d in sampled)})")

    # 2. Every model's matching block: the coordinates carried over by vertex position.
    meshes = []
    for m, blocks in enumerate(model_blocks):
        if len(blocks) != 1 or len(blocks[0]["draws"]) != len(ref_block["draws"]):
            report.append(f"model {m} ({MODEL_NAMES[m]}): {len(blocks)} {a.part} blocks: not matched (stock)")
            continue
        blk = blocks[0]
        if blk["scale"] != ref_block["scale"] or blk["cmds"] > 40:
            report.append(f"model {m} ({MODEL_NAMES[m]}): another texture scale or a long block: not matched (stock)")
            continue
        verts, worst, why = [], 0.0, None
        for k, d in enumerate(blk["draws"]):
            vs = rom.vtx(d["vtx"], d["n"])
            ref = ref_vertices[k]
            # The cap's distinct corner points (a vertex repeated for the stock chart's seams counts once), each box
            # normalised: the target's must match the sampled model's one to one, each clearly nearest its partner.
            pv, pr = sorted({v[:3] for v in vs}), sorted({v[:3] for v in ref})
            nv, nr = norm(pv), norm(pr)
            if len(pv) != len(pr):
                why = f"{len(pv)} corner points, the sampled cap has {len(pr)}"
                break
            pair = {}
            for i, p in enumerate(pv):
                dist = sorted((math.dist(nv[i], nr[j]), j) for j in range(len(pr)))
                worst = max(worst, dist[0][0])
                if dist[0][0] > 0.6 * dist[1][0]:
                    why = f"corner {p} is about as near two of the sampled cap's ({dist[0][0]:.2f} / {dist[1][0]:.2f})"
                    break
                pair[p] = pr[dist[0][1]]
            if why:
                break
            if len(set(pair.values())) != len(pr):
                why = "two corners take the same sampled corner"
                break
            uv_of = {}
            for v in ref:
                uv_of.setdefault(v[:3], (v[3], v[4]))
            new = [uv_of[pair[v[:3]]] for v in vs]

            # The design folds its chart over the cap (both mappings mix windings), so each triangle is checked
            # against the sampled cap's triangles across its edges: its texture triangle must be wound as one of
            # theirs is (a triangle flipped against all its neighbours would smear the stripes).
            def wind(p, q, r):
                return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
            ref_tris = [tuple(ref[i][:3] for i in t) for t in ref_block["draws"][k]["tris"]]
            ref_sign = [wind(*(uv_of[c] for c in t)) > 0 for t in ref_tris]
            bad = []
            for t in d["tris"]:
                corners = [pair[vs[i][:3]] for i in t]
                if len(set(corners)) < 3:
                    bad.append(t)
                    continue
                s = wind(*(new[i] for i in t)) > 0
                near = [ref_sign[j] for j, rt in enumerate(ref_tris) if len(set(rt) & set(corners)) >= 2]
                if s not in near:
                    bad.append(t)
            if bad:
                why = f"{len(bad)} triangle(s) wound against every neighbour on the sampled cap ({bad[:3]})"
                break
            verts.append({"at": f"{d['vtx']:08X}", "st": [[v[3], v[4]] for v in vs],
                          "uv": [[max(-32768, min(32767, round(u * to_raw[0]))), max(-32768, min(32767, round(w * to_raw[1])))]
                                 for u, w in new]})
        if why:
            report.append(f"model {m} ({MODEL_NAMES[m]}): not matched, stock texture with stock coordinates: {why}")
            continue
        meshes.append({"model": m, "block": f"{blk['block']:08X}", "end": f"{blk['end']:08X}", "vertices": verts})
        report.append(f"model {m} ({MODEL_NAMES[m]}): block {blk['block']:08X}, {sum(len(v['st']) for v in verts)} vertices, "
                      f"corner points matched one to one (largest normalised distance {worst:.3f})")

    # 3. The sets' textures and pack.json.
    pack_path = a.pack / "pack.json"
    pack = json.loads(pack_path.read_text())
    sys.path.insert(0, str(ROOT / "tools" / "announcer"))
    import make_league_pak as mlp
    added = a.pack / "files" / "added"
    src = a.table.parent
    written = []
    for tid, sets in table["sets"].items():
        team = next((t for t in pack["teams"] if t["id"] == tid), None)
        if team is None:
            raise SystemExit(f"{tid}: not a team of {pack_path}")
        for set_id in sets:
            if set_id in ("home", "away"):
                png = src / tid / f"{a.part}_{set_id}.png"
                stock = team.get("uniform_files", {}).get(f"{a.part}_{set_id}")
                holder = team.setdefault("uv_override", {})
                key = set_id
            else:
                png = src / tid / "alternates" / set_id / f"{a.part}.png"
                us = next((u for u in team.get("uniform_sets", []) if u["id"] == set_id), None)
                if us is None:
                    raise SystemExit(f"{tid}: no uniform set {set_id} in {pack_path}")
                stock = us["files"].get(a.part)
                holder, key = us, "uv_override"
            if not stock:
                raise SystemExit(f"{tid} {set_id}: the set has no {a.part} file of its own to key the override on")
            # The texture: the stock file's size, flags and origin (its game file's), exact palette.
            d = (a.pack / "files" / "added" / stock).read_bytes()
            typ, w, h, ox, oy = struct.unpack_from(">IIIii", d, 4)
            img = Image.open(png).convert("RGBA")
            if img.size != (w, h) or typ != 16:
                raise SystemExit(f"{png}: {img.size} (type {typ}); the stock {a.part} is {(w, h)}, CI4")
            name = stock.replace(f"_{part['base_code']}_", f"_{part['code']}_")
            if name == stock:
                raise SystemExit(f"{stock}: can't name its override texture")
            (added / name).write_bytes(mlp.encode_ci4_exact(img, d[3], ox, oy))
            entry = {"table": a.name, "file": name}
            if key == "uv_override":
                holder["uv_override"] = entry
            else:
                holder[key] = entry
            written.append(f"{tid} {set_id}: {stock} + {name}")
    pack["uv_overrides"] = {**pack.get("uv_overrides", {}), a.name: {
        "part": a.part,
        "source": {"table_sha256": hashlib.sha256(a.table.read_bytes()).hexdigest(), "mannequin_sha256": table["mannequin_sha256"]},
        "meshes": meshes,
    }}
    pack_path.write_text(json.dumps(pack, indent=1) + "\n")
    print("\n".join(report))
    print("\n".join(written))
    print(f"{pack_path}: \"uv_overrides\".{a.name}: {len(meshes)} model blocks, {len(written)} sets")


if __name__ == "__main__":
    main()
