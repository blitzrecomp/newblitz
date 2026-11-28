#!/usr/bin/env python3
"""A team-select player from an RDRAM dump of the running game, drawn from any side: what the game itself drew (its
meshes, texture coordinates and the texels in TMEM at each draw), for views the team-select camera never shows.

  .venv/bin/python tools/dump_views.py <app folder>/dumps/rdram_<t>.bin --out views.png [--side 0|1] [--target torso]
                                       [--label "CSU home"] [--nearest]

The dump is turned into a mannequin (tools/make_mannequin.py) in a scratch folder, then drawn front, back, overhead,
left and right with tools/uniform_preview.py's renderer, each part from the dump itself: a part loaded from the side's
uniform buffer from that buffer, anything else (e.g. a per-set texture from src/game/uv_overrides.cpp) as the texels
TMEM held when the game drew it.
"""
import argparse
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

VIEWS = [("Front", 0, 0), ("Back", 180, 0), ("Overhead", 0, 80), ("Left", 90, 0), ("Right", -90, 0)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dump", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--side", type=int, choices=[0, 1])
    ap.add_argument("--target", choices=["player", "torso"], default="torso")
    ap.add_argument("--label", default="")
    ap.add_argument("--nearest", action="store_true")
    ap.add_argument("--size", default="360x360")
    ap.add_argument("--frame-from", type=Path, default=ROOT / "build" / "uniform_mannequin" / "mannequin.json",
                    help="a mannequin whose body frame (front, up) orients the views (default the previewer's)")
    a = ap.parse_args(argv)
    import make_mannequin
    import uniform_preview as up
    with tempfile.TemporaryDirectory() as tmp:
        args = [str(a.dump), "--out", tmp, "--name", "dump"]
        if a.side is not None:
            args += ["--side", str(a.side)]
        make_mannequin.main(args)
        path = Path(tmp) / "dump.json"
        if a.frame_from and a.frame_from.exists():
            # The reference mannequin's body frame turned with this player's torso (the jersey's triangles, the same
            # model in both, matched by the best turn about the up axis), so "Front" faces his chest as in sheets drawn
            # from the reference (tools/uniform_preview.py, a design handoff's comparisons) whatever his idle pose.
            import json
            import math
            m = json.loads(path.read_text())
            ref = json.loads(a.frame_from.read_text())

            def jersey_points(mj):
                d = next(i for i, e in enumerate(mj["draws"]) if any(p.startswith("jersey") for p in e.get("parts", [])))
                return [v[:3] for t in mj["tris"] if t[0] == d for v in t[1:]]
            pr, pd = jersey_points(ref), jersey_points(m)
            f = ref["frame"]
            if len(pr) == len(pd):
                upv = f["up"]
                cr = [sum(p[k] for p in pr) / len(pr) for k in range(3)]
                cd = [sum(p[k] for p in pd) / len(pd) for k in range(3)]
                ax = f["front"]
                ay = [upv[1] * ax[2] - upv[2] * ax[1], upv[2] * ax[0] - upv[0] * ax[2], upv[0] * ax[1] - upv[1] * ax[0]]
                dot = lambda p, q: sum(x * y for x, y in zip(p, q))  # noqa: E731
                num = den = 0.0
                for p, q in zip(pr, pd):
                    rp = [p[k] - cr[k] for k in range(3)]
                    rq = [q[k] - cd[k] for k in range(3)]
                    x1, y1, x2, y2 = dot(rp, ax), dot(rp, ay), dot(rq, ax), dot(rq, ay)
                    num += x1 * y2 - y1 * x2
                    den += x1 * x2 + y1 * y2
                th = math.atan2(num, den)

                def turn(v):  # about up, from ax toward ay
                    x, y, z = dot(v, ax), dot(v, ay), dot(v, upv)
                    x, y = x * math.cos(th) - y * math.sin(th), x * math.sin(th) + y * math.cos(th)
                    return [x * ax[k] + y * ay[k] + z * upv[k] for k in range(3)]
                off = turn([f["origin"][k] - cr[k] for k in range(3)])
                m["frame"] = dict(f, origin=[cd[k] + off[k] for k in range(3)], front=turn(f["front"]), right=turn(f["right"]))
                print(f"frame: the reference's, turned {math.degrees(th):.0f} degrees with the torso")
            path.write_text(json.dumps(m))
        man = up.Mannequin(path)
    w, h = (int(x) for x in a.size.split("x"))
    # The torso: the jersey's draws (named jersey/<slot> in a dump), framed loosely enough for both shoulders.
    parts = sorted({p for e in man.draws for p in e.get("parts", []) if p.startswith("jersey")}) if a.target == "torso" else None
    tiles = []
    for name, yaw, pitch in VIEWS:
        cam = up.fit_camera(man, parts, yaw, (w, h), pitch=pitch, dist_scale=1.5 if parts else 1.1)
        im = up.render(man, man.tris, cam, (w, h), dump_uniform=man.m.get("dump_uniform"), bg=up.PANEL,
                       bilinear=not a.nearest, supersample=2)
        tiles.append(up.labelled(im, name))
    rows = [up.row(a.label or a.dump.name, tiles, note=f"from {a.dump.name}: the game's own meshes, coordinates and TMEM texels")]
    # The helmet's two physical sides: at the game's on-field size (the previewer's, filtered as the game does), then 4x.
    hs = []
    for name, yaw in (("Helmet, left side", 90), ("Helmet, right side", -90)):
        n = up.GAME_HELMET_PX + 6
        small = up.render(man, man.tris, up.fit_camera(man, up.HELMET, yaw, (n, n), fov=20, margin=1.0), (n, n),
                          dump_uniform=man.m.get("dump_uniform"), bg=up.PANEL, supersample=4)
        big = up.render(man, man.tris, up.fit_camera(man, up.HELMET, yaw, (256, 256), fov=20, margin=1.0), (256, 256),
                        dump_uniform=man.m.get("dump_uniform"), bilinear=False, bg=up.PANEL)
        hs += [up.labelled(small, name, "game size"), up.labelled(big, name, "4x, nearest")]
    rows.append(up.row("Helmet", hs))
    sheet = up.stack(rows)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(a.out)
    print(a.out)


if __name__ == "__main__":
    main()
