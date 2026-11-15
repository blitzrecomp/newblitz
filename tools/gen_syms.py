#!/usr/bin/env python3
"""Generate the N64Recomp symbol file for NFL Blitz 2000 (v1.1).

Output: build/syms/blitz2000.syms.toml (generated from the ROM, so kept out of git).

  1. Function starts in each segment's text come from spimdisasm.
  2. libultra/libc functions are named by n64sym's signature database (tools/sigmatch.py).
     A match counts only at a function start, and only if the bytes left over in the
     function are padding. Calls inside a matched function name their targets too.
  3. syms/names.toml (hand-identified names) is applied last and wins.

Prints a report: coverage of the libultra names N64Recomp reimplements or ignores,
names matched at more than one address, and unnamed functions in the libultra range.

Usage: .venv/bin/python tools/gen_syms.py [--rom R] [--out O] [--dis D] [--names EXTRA.toml ...]
  Defaults: build/baserom.z64 -> build/syms/blitz2000.syms.toml (Classic). The 2026 flavour passes
  build/teamplus.z64, its own output and disassembly cache, and syms/names_teamplus.toml (merged
  over names.toml).
"""
import argparse
import re
import subprocess
import sys

import rabbitizer
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sigmatch  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ROM = ROOT / "build" / "baserom.z64"
DIS = ROOT / "build" / "dis"
OUT = ROOT / "build" / "syms" / "blitz2000.syms.toml"
NAMES = ROOT / "syms" / "names.toml"
SYMBOL_LISTS = ROOT / "lib" / "N64Recomp" / "src" / "symbol_lists.cpp"

# name, rom start, vram, text end (vram). See NOTES.md "ROM map" for how each was found.
SEGMENTS = [
    # Boot: IPL3 copies ROM 0x1000.. to 0x80000400. CPU text ends where rspboot begins (0x28740);
    # after it come the RSP microcodes (not CPU code), then .data/.rodata to 0x8002F020.
    ("boot", 0x00001000, 0x80000400, 0x80027B40),
    # Main: load_main_segment DMAs ROM 0x2FC20-0x93EC0; it invalidates the I-cache for exactly this text range.
    ("main", 0x0002FC20, 0x80248F00, 0x80281EB0),
    # Overlays, all at 0x802EFE70; text ends after the last `jr ra` (16-byte aligned).
    ("ovl_a", 0x00093EC0, 0x802EFE70, 0x80333820),
    ("ovl_b", 0x000DE350, 0x802EFE70, 0x80310D30),
    ("ovl_c", 0x00129A90, 0x802EFE70, 0x802F4320),
    # Stadium blobs, ROM 0x12E860-0x150490, all at 0x80377D20 (ovl_a's bss end). ovl_a 0x803244AC DMAs
    # the current stadium's blob there after osInvalICache on exactly this code range; ovl_a 0x8032421C
    # then calls its one function, which loads the stadium's textures into 0x8033BA80 (segment 0xC).
    # The rest of each blob (to the next one) is the stadium's display lists and vertices.
    ("ovl_stadium1", 0x0012E860, 0x80377D20, 0x80378040),
    ("ovl_stadium2", 0x001348D0, 0x80377D20, 0x80377F70),
    ("ovl_stadium3", 0x0013BEB0, 0x80377D20, 0x80378130),
    ("ovl_stadium4", 0x00142B30, 0x80377D20, 0x80377FF0),
    ("ovl_stadium5", 0x00148C60, 0x80377D20, 0x80377FC0),
]

# Signatures shorter than this match trivial stubs and getters all over the game, so they are
# only accepted when they match a single place in the whole game.
MIN_SIG_SIZE = 0x20

# Where each segment's initialised data ends in ROM (text end .. here is .data/.rodata),
# scanned for function pointer tables.
SEGMENT_ROM_END = {"boot": 0x2FC20, "main": 0x93EC0, "ovl_a": 0xDE350, "ovl_b": 0x129A90, "ovl_c": 0x12E860,
                   # Stadium blobs: what follows the code is display lists, not pointer tables.
                   "ovl_stadium1": 0x12EB80, "ovl_stadium2": 0x134B20, "ovl_stadium3": 0x13C2C0,
                   "ovl_stadium4": 0x142E00, "ovl_stadium5": 0x148F00}

# The libultra part of boot (link order puts it after the game's own boot code).
LIBULTRA_RANGE = (0x80017000, 0x80027B40)


def function_starts(name: str, rom_start: int, vram: int, text_end: int) -> list[int]:
    out_dir = DIS / f"{name}_text"
    asm = list(out_dir.glob("*.text.s"))
    if not asm:
        rom_end = rom_start + (text_end - vram)
        subprocess.run([sys.executable, "-m", "spimdisasm", "singleFileDisasm", str(ROM), str(out_dir),
                        "--start", hex(rom_start), "--end", hex(rom_end), "--vram", hex(vram)],
                       check=True, capture_output=True)
        asm = list(out_dir.glob("*.text.s"))
    text = asm[0].read_text()
    return sorted({int(a, 16) for a in re.findall(r"^glabel \w*?([0-9A-F]{8})\b", text, re.M)} | {vram})


def padded_starts(seg: bytes, vram: int) -> set[int]:
    """Code that resumes after `jr ra`, its delay slot and at least one word of zero padding:
    the padding is the linker aligning the next function. Wrong guesses are undone by merge_split_functions."""
    out = set()
    jr_ra = b"\x03\xe0\x00\x08"
    for o in range(0, len(seg) - 12, 4):
        if seg[o:o + 4] == jr_ra and seg[o + 8:o + 12] == b"\0\0\0\0":
            j = o + 8
            while j < len(seg) and seg[j:j + 4] == b"\0\0\0\0":
                j += 4
            if j < len(seg):
                out.add(vram + j)
    return out


JAL_SOURCES = {}  # target -> set of segment names containing a jal to it


def segment_owns_jal(name: str, target: int) -> bool:
    """A jal target in shared overlay VRAM is only trusted in the overlay the jal came from
    (a jal in ovl_a says nothing about what is at that address in ovl_b)."""
    srcs = JAL_SOURCES.get(target, set())
    if not name.startswith("ovl_"):
        return True
    return name in srcs


def jal_targets(rom: bytes) -> set[int]:
    """Every jal target in every segment's text. spimdisasm sees one segment at a time,
    so functions only called from another segment would otherwise be missed."""
    out = set()
    for name, rom_start, vram, text_end in SEGMENTS:
        for i in range(rom_start, rom_start + (text_end - vram), 4):
            w = int.from_bytes(rom[i:i + 4], "big")
            if w >> 26 == 3:
                t = 0x80000000 | ((w & 0x03FFFFFF) << 2)
                out.add(t)
                JAL_SOURCES.setdefault(t, set()).add(name)
    return out


def pointer_targets(rom: bytes) -> set[int]:
    """Addresses that code builds with lui/addiu (or lui/ori) pairs, plus aligned words in each
    segment's data: thread entries, callbacks and function pointer tables that no jal reaches."""
    out = set()
    for name, rom_start, vram, text_end in SEGMENTS:
        text_rom_end = rom_start + (text_end - vram)
        his = {}  # register -> (upper half, index of the lui)
        for i in range(rom_start, text_rom_end, 4):
            w = int.from_bytes(rom[i:i + 4], "big")
            op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 0x0F:  # lui
                his[rt] = (imm, i)
            elif op in (0x09, 0x0D) and rs in his and i - his[rs][1] <= 0x40:  # addiu / ori
                lo = imm - 0x10000 if (op == 0x09 and imm & 0x8000) else imm
                out.add(((his[rs][0] << 16) + lo) & 0xFFFFFFFF)
        for i in range(text_rom_end, SEGMENT_ROM_END[name], 4):
            out.add(int.from_bytes(rom[i:i + 4], "big"))
    return {a for a in out if 0x80000400 <= a < 0x80400000 and a % 4 == 0}


def is_delay_slot(seg: bytes, off: int) -> bool:
    """True if the word before off is a jump or branch, i.e. off is its delay slot."""
    if off < 4:
        return False
    prev = rabbitizer.Instruction(int.from_bytes(seg[off - 4:off], "big"))
    return prev.isJump() or prev.isBranch()


def at_boundary(seg: bytes, off: int) -> bool:
    """Code resumes after `jr ra` + its delay slot, optionally followed by zero padding."""
    if off == 0:
        return True
    j = off
    while j >= 4 and seg[j - 4:j] == b"\0\0\0\0":
        j -= 4
    jr_ra = b"\x03\xe0\x00\x08"
    if seg[j - 8:j - 4] == jr_ra:          # j-4 is the (non-zero) delay slot
        return True
    return j < off and seg[j - 4:j] == jr_ra  # the delay slot was a nop, consumed as padding


def looks_like_start(seg: bytes, off: int, trusted: bool = False) -> bool:
    """Whether a harvested target can be a function start. Never a delay slot. Real jal targets
    (trusted) are accepted anywhere else; values that merely look like pointers (data words,
    lui/addiu pairs) must sit at a boundary, because GCC often schedules loads before the
    `addiu sp` of a prologue, so an `addiu sp` alone can be mid-function."""
    if is_delay_slot(seg, off):
        return False
    if trusted:
        return True
    return at_boundary(seg, off)


def first_invalid(seg: bytes, start: int, end: int, vram: int) -> int | None:
    """VRAM of the first word in [start, end) that is not a valid CPU instruction."""
    for a in range(start, end, 4):
        o = a - vram
        if not rabbitizer.Instruction(int.from_bytes(seg[o:o + 4], "big"), vram=a).isValid():
            return a
    return None


def trim_data_islands(seg: bytes, vram: int, bounds: list[tuple[int, int]], called: set[int] = frozenset()):
    """Cut each function at its last `jr ra` (+ delay slot) before any undecodable word.
    Returns (bounds, islands); islands are (start, end) ranges dropped as data.

    Code can resume inside an island: if everything after the island's last undecodable word is
    valid and the game calls or points at its first non-zero word, that tail is a function."""
    out, islands = [], []
    for start, end in bounds:
        bad = first_invalid(seg, start, end, vram)
        if bad is None:
            out.append((start, end))
            continue
        cut = None
        for a in range(bad - 4, start - 4, -4):
            if seg[a - vram:a - vram + 4] == b"\x03\xe0\x00\x08":
                cut = a + 8
                break
        island_start = cut if cut is not None else start
        if cut is not None:
            out.append((start, cut))
        last_bad = max(a for a in range(island_start, end, 4)
                       if not rabbitizer.Instruction(int.from_bytes(seg[a - vram:a - vram + 4], "big"), vram=a).isValid())
        resume = last_bad + 4
        while resume < end and seg[resume - vram:resume - vram + 4] == b"\0\0\0\0":
            resume += 4
        if resume < end and resume in called:
            islands.append((island_start, resume))
            out.append((resume, end))
        else:
            islands.append((island_start, end))
    return out, islands


def merge_split_functions(seg: bytes, vram: int, bounds: list[tuple[int, int]], keep: set[int]):
    """Merge neighbours when a branch in one lands inside another: the split was wrong
    (spimdisasm sometimes labels a loop head inside a function). Starts in `keep` are never merged away."""
    changed = True
    merged = []
    while changed:
        changed = False
        starts = {b[0] for b in bounds}
        for i, (start, end) in enumerate(bounds):
            lo, hi = start, end
            for a in range(start, end, 4):
                ins = rabbitizer.Instruction(int.from_bytes(seg[a - vram:a - vram + 4], "big"), vram=a)
                if ins.isBranch():
                    t = ins.getBranchVramGeneric()
                    if t in starts and not start <= t < end:
                        continue  # branch to another function's start: a tail call, not a bad split
                    lo, hi = min(lo, t), max(hi, t + 4)
            if lo == start and hi <= end:
                continue
            group = [j for j, (s2, e2) in enumerate(bounds) if s2 < hi and e2 > lo]
            if any(bounds[j][0] in keep for j in group if bounds[j][0] != bounds[group[0]][0]):
                continue
            new = (bounds[group[0]][0], bounds[group[-1]][1])
            merged += [bounds[j][0] for j in group[1:]]
            bounds = bounds[:group[0]] + [new] + bounds[group[-1] + 1:]
            changed = True
            break
    return bounds, merged


def ends_cleanly(seg: bytes, vram: int, start: int, end: int) -> bool:
    """True if control can't run off the end: the function's last real instruction (before any zero
    padding) is in the delay slot of an unconditional jump/branch, or is such a jump."""
    a = end - 4
    while a >= start and seg[a - vram:a - vram + 4] == b"\0\0\0\0":
        a -= 4
    for cand in (a - 4, a):  # the jump itself, or (if the delay slot was a nop) the last word
        if cand < start:
            continue
        ins = rabbitizer.Instruction(int.from_bytes(seg[cand - vram:cand - vram + 4], "big"), vram=cand)
        name = ins.getOpcodeName()
        if name in ("jr", "j", "b", "eret") or (name == "beq" and ins.disassemble().count("$zero") >= 2):
            return True
    return False


def merge_fallthroughs(seg: bytes, vram: int, bounds: list[tuple[int, int]], referenced: set[int]):
    """A function that runs off its end continues into the next one. If the next start is not
    referenced on its own (no jal, pointer or hand name), they are one function: merge. Classic case:
    a call to an alignment nop just before the real body (jal 0x8000B4E0 -> nop; body at 0x8000B4E4).
    Returns (bounds, merged starts, unresolved: falls through into a referenced start)."""
    out, merged, unresolved = [], [], []
    for start, end in bounds:
        if out and not ends_cleanly(seg, vram, *out[-1]) and start not in referenced:
            out[-1] = (out[-1][0], end)
            merged.append(start)
            continue
        if out and not ends_cleanly(seg, vram, *out[-1]):
            unresolved.append((out[-1][0], start))
        out.append((start, end))
    return out, merged, unresolved


def is_padding(data: bytes) -> bool:
    return all(b == 0 for b in data)


def runtime_name_lists() -> tuple[set, set]:
    if not SYMBOL_LISTS.exists():
        return set(), set()
    src = SYMBOL_LISTS.read_text()

    def block(name):
        m = re.search(name + r"\s*\{(.*?)\n\};", src, re.S)
        return set(re.findall(r'"([A-Za-z_0-9]+)"', m.group(1))) if m else set()
    return block("reimplemented_funcs"), block("ignored_funcs")


def count_short_matches(rom: bytes, index) -> dict[str, int]:
    """How many function starts each short signature matches across the whole game."""
    counts = {}
    for name, rom_start, vram, text_end in SEGMENTS:
        seg = rom[rom_start:rom_start + (text_end - vram)]
        for off in function_starts(name, rom_start, vram, text_end):
            best = index.best_match(seg, off - vram)
            if best and best.size < MIN_SIG_SIZE:
                counts[best.name] = counts.get(best.name, 0) + 1
    return counts


def main(argv=None) -> int:
    global ROM, DIS, OUT
    ap = argparse.ArgumentParser(description="Generate the N64Recomp symbol file")
    ap.add_argument("--rom", default=str(ROM))
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--dis", default=str(DIS), help="spimdisasm cache directory (one per ROM)")
    ap.add_argument("--names", action="append", default=[], help="extra names toml, merged over syms/names.toml")
    args = ap.parse_args(argv)
    ROM, OUT, DIS = Path(args.rom).resolve(), Path(args.out).resolve(), Path(args.dis).resolve()
    rom = ROM.read_bytes()
    index = sigmatch.SigIndex(sigmatch.load_signatures())
    # Short signatures (tiny getters/stubs) are trusted only when they match exactly one place.
    short_counts = count_short_matches(rom, index)
    short_ok = lambda n: short_counts.get(n, 0) == 1
    short_hits = {}
    hand = tomllib.loads(NAMES.read_text()) if NAMES.exists() else {}
    for extra in args.names:
        for sec, entries in tomllib.loads(Path(extra).read_text()).items():
            hand.setdefault(sec, {}).update(entries)
    reimpl, ignored = runtime_name_lists()

    jals = jal_targets(rom)
    called = jals | pointer_targets(rom)

    sections = []
    all_islands = []
    all_merged = []
    all_fallthrough = []
    all_unresolved = []
    all_named = {}  # name -> [(section, vram)]
    report_unnamed_lib = []
    for name, rom_start, vram, text_end in SEGMENTS:
        starts = function_starts(name, rom_start, vram, text_end)
        hand_names = {int(k, 0) if isinstance(k, str) else k: v for k, v in hand.get(name, {}).items()}
        seg = rom[rom_start:rom_start + (text_end - vram)]
        # Call targets harvested from other segments (or from data that decodes as jal) are only
        # trusted where a function plausibly begins.
        extra = {a for a in called if vram <= a < text_end and a % 4 == 0
                 and looks_like_start(seg, a - vram, trusted=a in jals and segment_owns_jal(name, a))}
        starts = sorted(set(starts) | extra | padded_starts(seg, vram) | {a for a in hand_names if vram <= a < text_end})
        bounds = list(zip(starts, starts[1:] + [text_end]))
        bounds, islands = trim_data_islands(seg, vram, bounds, called)
        bounds, merged = merge_split_functions(seg, vram, bounds, set(hand_names))
        all_merged += [(name, a) for a in merged]
        bounds, merged, unresolved = merge_fallthroughs(seg, vram, bounds, called | set(hand_names))
        all_fallthrough += [(name, a) for a in merged]
        all_unresolved += [(name, a, b) for a, b in unresolved]
        all_islands += [(name, a, b) for a, b in islands]

        names = {}
        for start, end in bounds:
            off = start - vram
            best = index.best_match(seg, off, max_size=end - start)
            if best and best.size < MIN_SIG_SIZE:
                short_hits.setdefault(best.name, []).append((name, start))
                if not short_ok(best.name):
                    best = None
            if best and is_padding(seg[off + best.size:end - vram]):
                names[start] = best.name
                # name call targets from the signature's relocations (n64sym does the same)
                for roff, (kind, target) in best.relocs.items():
                    if kind != ".targ26" or re.search(r"_(text|data|rodata|bss)_[0-9A-F]{4}$", target):
                        continue
                    w = int.from_bytes(seg[off + roff:off + roff + 4], "big")
                    tgt = (start & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    if vram <= tgt < text_end:
                        names.setdefault(tgt, target)
        names.update(hand_names)

        funcs = []
        for start, end in bounds:
            # Overlays share VRAM, so their default names get the section as a prefix: identical
            # names in different sections collapse to one definition at link time.
            default = f"{name}_func_{start:08X}" if name.startswith("ovl_") else f"func_{start:08X}"
            fname = names.get(start, default)
            funcs.append((fname, start, end - start))
            if start in names:
                all_named.setdefault(fname, []).append((name, start))
            elif name == "boot" and LIBULTRA_RANGE[0] <= start < LIBULTRA_RANGE[1]:
                report_unnamed_lib.append(start)
        sections.append((name, rom_start, vram, text_end - vram, funcs))

    # Duplicate names break N64Recomp; keep the first and suffix the rest so the file stays valid.
    dups = {n: locs for n, locs in all_named.items() if len(locs) > 1}
    for n, locs in dups.items():
        for sec_name, addr in locs[1:]:
            for i, (sname, *_rest) in enumerate(sections):
                if sname == sec_name:
                    funcs = sections[i][4]
                    sections[i][4][:] = [(f"{f}_{a:08X}" if f == n and a == addr else f, a, s) for f, a, s in funcs]

    OUT.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# Generated by tools/gen_syms.py from the pinned ROM. Do not edit; edit syms/names.toml.", ""]
    for name, rom_start, vram, size, funcs in sections:
        lines += ["[[section]]", f'name = "{name}"', f"rom = 0x{rom_start:08X}", f"vram = 0x{vram:08X}",
                  f"size = 0x{size:X}", "", "functions = ["]
        lines += [f'    {{ name = "{f}", vram = 0x{a:08X}, size = 0x{s:X} }},' for f, a, s in funcs]
        lines += ["]", ""]
    OUT.write_text("\n".join(lines))

    # Report
    total = sum(len(s[4]) for s in sections)
    named = sum(1 for s in sections for f, *_ in s[4] if not re.fullmatch(r"(ovl_\w+_)?func_[0-9A-F]{8}", f))
    # recomp.h makes every recompiled function a weak symbol, so two sections defining the same
    # name would silently share one body at link time. Refuse to write such a file.
    seen = {}
    for sname, *_rest, funcs in sections:
        for f, a, _ in funcs:
            if f in seen:
                raise SystemExit(f"duplicate function name {f}: {seen[f]} and {sname}:0x{a:08X}")
            seen[f] = f"{sname}:0x{a:08X}"
    shown = OUT.relative_to(ROOT) if OUT.is_relative_to(ROOT) else OUT
    print(f"Wrote {shown}: {len(sections)} sections, {total} functions, {named} named")
    for name, _, vram, size, funcs in sections:
        print(f"  {name:6} 0x{vram:08X}+0x{size:05X}  {len(funcs):5} functions")
    found = {f for s in sections for f, *_ in s[4]}
    if reimpl or ignored:
        print(f"Runtime-replaced libultra names present: reimplemented {len(found & reimpl)}, ignored {len(found & ignored)}")
    if dups:
        print("Names matched at more than one address (need a hand decision in syms/names.toml):")
        for n, locs in sorted(dups.items()):
            print(f"  {n}: " + ", ".join(f"{s}:0x{a:08X}" for s, a in locs))
    if all_merged:
        print("Wrong splits merged (a branch crossed into the next function):")
        for sec, a in all_merged:
            print(f"  {sec:6} 0x{a:08X}")
    if all_fallthrough:
        print(f"Fall-through starts merged into the function before them: {len(all_fallthrough)}")
        for sec, a in all_fallthrough:
            print(f"  {sec:6} 0x{a:08X}")
    if all_unresolved:
        print("WARNING: functions that run off their end into a separately referenced function:")
        for sec, a, b in all_unresolved:
            print(f"  {sec:6} 0x{a:08X} -> 0x{b:08X}")
    if all_islands:
        print("Data islands inside text (excluded from functions):")
        for sec, a, b in all_islands:
            print(f"  {sec:6} 0x{a:08X}-0x{b:08X} (0x{b - a:X} bytes)")
    print(f"Unnamed functions in libultra range 0x{LIBULTRA_RANGE[0]:08X}-0x{LIBULTRA_RANGE[1]:08X}: {len(report_unnamed_lib)}")
    for a in report_unnamed_lib:
        print(f"  0x{a:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
