#!/usr/bin/env python3
"""M6 check (docs/PSP_PORT.md 4.5): the PSP renderer's display-list walker against tools/gbi_dump.py's decode.

Runs gbi_dump.py's Walker (the reference) and build/psp/gfxtest/gfx-dltest (the renderer's own walker on the Mac, null GE
backend) over each dump and compares: total commands, every per-opcode count, vertices loaded, triangles submitted,
unknown opcodes (must be 0). Exits 1 on any difference.

  tools/psp/gbi_check.py [build/dumps/rdram_543.bin ...]     (default: the five M6 dumps)
"""
import importlib.util
import re
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
DUMPS = [ROOT / f"build/dumps/rdram_{n}.bin" for n in (543, 1439, 1448, 1529, 1538)]
DLTEST = ROOT / "build/psp/gfxtest/gfx-dltest"


def load_gbi_dump():
    spec = importlib.util.spec_from_file_location("gbi_dump", ROOT / "tools/gbi_dump.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["gbi_dump"] = mod   # dataclasses look their module up
    spec.loader.exec_module(mod)
    return mod


def reference(g, path: Path) -> dict:
    side = path.with_suffix(".txt").read_text()
    dl = int(re.search(r"dlist=([0-9A-Fa-f]+)", side).group(1), 16)
    args = SimpleNamespace(cmds=False, vertices=False, matrices=False, follow=True, cull="eval", branch_z="eval",
                           max_cmds=200000)
    w = g.Walker(g.Rdram(path), args)
    counts = {"vtx_loaded": 0, "tris": 0}
    orig = w.exec

    def exec_counting(op, w0, w1, addr, depth):
        if op == g.G_VTX:
            n, v0 = g.bits(w0, 12, 8), g.bits(w0, 1, 7) - g.bits(w0, 12, 8)
            counts["vtx_loaded"] += sum(1 for i in range(n) if 0 <= v0 + i < w.st.vtx_size)
        elif op == g.G_TRI1:
            counts["tris"] += 1
        elif op in (g.G_TRI2, g.G_QUAD):
            counts["tris"] += 2
        return orig(op, w0, w1, addr, depth)

    w.exec = exec_counting
    w.run(dl)
    known = set(g.OPNAMES) - {0x100}
    return {"cmds": w.ncmds, "ops": {op: n for op, n in w.op_counts.items()},
            "unknown_ops": sum(n for op, n in w.op_counts.items() if op not in known and op != 0x100), **counts}


def ours(path: Path) -> dict:
    out = subprocess.run([str(DLTEST), str(path)], capture_output=True, text=True, check=True).stdout
    dec = re.search(r"^decode (.*)$", out, re.M).group(1)
    kv = dict(x.split("=") for x in dec.split())
    ops = dict(x.split("=") for x in re.search(r"^ops (.*)$", out, re.M).group(1).split())
    return {"cmds": int(kv["cmds"]), "vtx_loaded": int(kv["vtx_loaded"]), "tris": int(kv["tris"]),
            "unknown_ops": int(kv["unknown_ops"]), "ops": {int(k, 16): int(v) for k, v in ops.items()}}


def main() -> int:
    paths = [Path(p) for p in sys.argv[1:]] or DUMPS
    if not DLTEST.exists():
        sys.exit(f"{DLTEST} missing: make psp-gfxtest")
    g = load_gbi_dump()
    bad = 0
    for p in paths:
        r, o = reference(g, p), ours(p)
        diffs = [f"{k}: ref {r[k]} ours {o[k]}" for k in ("cmds", "vtx_loaded", "tris", "unknown_ops") if r[k] != o[k]]
        for op in sorted(set(r["ops"]) | set(o["ops"])):
            if r["ops"].get(op, 0) != o["ops"].get(op, 0):
                diffs.append(f"op {g.OPNAMES.get(op, hex(op))}: ref {r['ops'].get(op, 0)} ours {o['ops'].get(op, 0)}")
        status = "ok" if not diffs else "DIFF"
        print(f"{p.stem}: {status} cmds={o['cmds']} vtx_loaded={o['vtx_loaded']} tris={o['tris']} "
              f"opcodes={len(o['ops'])} unknown_ops={o['unknown_ops']} (ref unknown {r['unknown_ops']})")
        for d in diffs:
            print(f"   {d}")
        bad += bool(diffs) or o["unknown_ops"] != 0
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
