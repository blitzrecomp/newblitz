#!/usr/bin/env python3
"""Which coordinates each player's body got in a dumped frame (src/game/uv_overrides.cpp): per player, his side's
uniform buffer, his model (main's six player models) and segment 15's lists (stock, or a set's), and whether each of
his shoulder caps was drawn with the set's texture and coordinates.

  .venv/bin/python tools/uvo_check.py <app folder>/dumps/rdram_<t>.bin [--pack <pack folder>]

With --pack, each set's lists are named from pack.json's order (the app numbers them as it reads the sets), and every
cap drawn from a set's list is checked: its vertices' S/T equal the table's and its texture the set's .wms.
"""
import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODELS = {0x80294498: "normal high", 0x80296590: "normal mid", 0x80297FB8: "normal low",
          0x80299078: "big high", 0x8029AD60: "big mid", 0x8029C6C8: "big low"}
SIDES = {0x2BE4B0: 0, 0x2C4590: 1}
ENTRIES = 0x806E8000
# The region's layout by its magic word at 0x806E0000: "UVO1" (before per-cap textures) and "UVO2": the stock lists'
# address and one list's stride; an entry's lists are a list per listed block (UVO1: always 16 slots).
LAYOUTS = {0x55564F31: (0x806E1000, 0x200, 16), 0x55564F32: (0x806E0100, 0x280, None)}


def words(mem, a, n):
    a &= 0x1FFFFFFF
    return [struct.unpack("<I", mem[a + 4 * i:a + 4 * i + 4])[0] for i in range(n)]


def be_bytes(mem, a, n):
    a &= 0x1FFFFFFF
    out = bytearray()
    for i in range(a & ~3, a + n + 4, 4):
        out += mem[i:i + 4][::-1]
    return bytes(out[a & 3:(a & 3) + n])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dump", type=Path)
    ap.add_argument("--pack", type=Path)
    a = ap.parse_args(argv)
    txt = a.dump.with_suffix(".txt").read_text()
    dl = re.search(r"dlist=([0-9A-Fa-f]+)", txt).group(1)
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "gbi_dump.py"), str(a.dump), dl, "--cmds"],
                         capture_output=True, text=True).stdout
    mem = a.dump.read_bytes()
    names = {}
    magic = words(mem, 0x806E0000, 1)[0]
    if magic not in LAYOUTS:
        print(f"{a.dump.name}: no per-set lists in this frame (magic {magic:08X})")
    STOCK, stride, slots = LAYOUTS.get(magic, LAYOUTS[0x55564F32])
    blocks = []   # the app's mesh slots: every table's blocks in order, each once
    if a.pack:
        pack = json.loads((a.pack / "pack.json").read_text())
        for _, t in sorted(pack.get("uv_overrides", {}).items()):   # the app reads the tables by name
            for m in t["meshes"]:
                if m["block"] not in blocks:
                    blocks.append(m["block"])
    ENTRY_STRIDE = stride * (slots or max(1, len(blocks)))
    if a.pack:
        e = 0
        for t in pack["teams"]:   # the app's order: per team, its sets by name (std::map: alternates, "away", "home")
            sets = dict(t.get("uv_override", {}))
            for s in t.get("uniform_sets", []):
                if "uv_override" in s:
                    sets[s["id"]] = s["uv_override"]
            for k in sorted(sets):
                names[e] = (t["id"], k, sets[k])
                e += 1
    side, seg15, players = None, None, []
    for line in out.splitlines():
        m = re.search(r"DB060030 ([0-9A-F]{8})", line)
        if m:
            side = SIDES.get(int(m.group(1), 16) & 0x1FFFFFFF)
        m = re.search(r"DB06003C ([0-9A-F]{8})", line)
        if m:
            seg15 = int(m.group(1), 16) | 0x80000000
        m = re.search(r"G_DL call 0x([0-9A-F]{8})", line)
        if m and (int(m.group(1), 16) | 0x80000000) in MODELS:
            body = int(m.group(1), 16) | 0x80000000
            if seg15 is None:
                lists = "none set"
            elif seg15 == STOCK:
                lists = "stock"
            elif seg15 >= ENTRIES:
                k = (seg15 - ENTRIES) // ENTRY_STRIDE
                lists = f"set {k}" + (f" ({names[k][0]} {names[k][1]})" if k in names else "")
            else:
                lists = f"? {seg15:08X}"
            players.append((side, MODELS[body], lists))
            seg15 = None
    branches = len(re.findall(r"G_DL branch 0x006E[0-9A-F]{4} \(seg 0F", out))
    print(f"{a.dump.name}: {len(players)} player bodies; {branches} cap blocks drawn through segment 15")
    for i, (s, model, lists) in enumerate(players):
        print(f"  player {i:2d}: side {s}, {model:11s}, {lists}")
    # Every cap block drawn from a set's list: its vertex loads' S/T and, cap by cap, its texture against the pak (the
    # cap's own texture where the set's "caps" name it, else the set's "file", else the side's own: no load).
    if a.pack:
        uv = {m["block"]: (t, m) for t in pack.get("uv_overrides", {}).values() for m in t["meshes"]}
        checked = bad = caps_seen = 0
        for blk in re.finditer(r"([0-9A-F]{8}): DE010000 0F([0-9A-F]{6})\s+G_DL branch 0x([0-9A-F]{8})", out):
            block, target = blk.group(1), int(blk.group(3), 16) | 0x80000000
            if target < ENTRIES or block not in uv:
                continue
            table, mesh = uv[block]
            k = (target - ENTRIES) // ENTRY_STRIDE
            ws = words(mem, target, 2 * (stride // 8))
            tex, mtx, draws = None, None, []   # per G_VTX: (the cap's G_MTX, the texture loaded, the buffer)
            for i in range(0, len(ws), 2):
                w0, w1 = ws[i], ws[i + 1]
                if w0 == 0xDE010000 and i:
                    break
                if w0 == 0xFD500000:
                    tex = w1
                elif w0 >> 24 == 0xDA:
                    mtx = w1
                elif w0 >> 24 == 0x01:
                    draws.append((mtx, tex, w1))
            ok = True
            for (_, _, v), ref in zip(draws, mesh["vertices"]):
                for j, (s_, t_) in enumerate(ref["uv"]):
                    S, T = struct.unpack(">hh", be_bytes(mem, (v | 0x80000000) + 16 * j + 8, 4))
                    ok &= (S, T) == (s_, t_)
            if k in names:
                ref = names[k][2]
                cap_of = {int(x, 16): c for c, x in table.get("caps", {}).items()}
                for m_, t_, _ in draws:
                    want = ref.get("caps", {}).get(cap_of.get(m_)) or ref.get("file")
                    caps_seen += cap_of.get(m_) in ref.get("caps", {})
                    if want is None:
                        ok &= t_ is None or t_ >> 24 == 0x0C   # the side's own (segment 12) or nothing loaded
                        continue
                    wmsb = (a.pack / "files" / "added" / want).read_bytes()
                    ok &= t_ is not None and be_bytes(mem, t_ | 0x80000000, len(wmsb) - 32) == wmsb[32:]
            checked += 1
            bad += not ok
        print(f"  set lists checked against the pak: {checked} block draws ({caps_seen} caps with their own texture), "
              f"{bad} differ")


if __name__ == "__main__":
    main()
