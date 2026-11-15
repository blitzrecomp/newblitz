#!/usr/bin/env python3
"""Flag recompiled functions that touch N64 hardware directly.

The runtime replaces or skips libultra functions by name (N64Recomp's reimplemented/ignored
lists). Anything else gets recompiled as-is, so hardware access left in it would run against
the PC's memory instead. This lists every function that is NOT runtime-handled and still contains:
  - cop0 moves, cache, eret, TLB ops
  - lui of a KSEG1 address (0xA0xx-0xBFxx): uncached RDRAM (the runtime only maps KSEG0
    RDRAM), RCP registers, PI/cart
Each hit needs a name (so the runtime handles it), a patch, or a reason it's harmless.

Usage: .venv/bin/python tools/hazards.py [--rom R] [--syms S] [--config C]   (after tools/gen_syms.py)
"""
import argparse
import re
import sys
import tomllib
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_syms import OUT, ROM, runtime_name_lists  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "blitz2000.toml"

HW_IDS = {"mfc0", "mtc0", "dmfc0", "dmtc0", "cache", "eret", "tlbp", "tlbr", "tlbwi", "tlbwr", "cfc0", "ctc0"}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Flag recompiled functions that touch N64 hardware")
    ap.add_argument("--rom", default=str(ROM))
    ap.add_argument("--syms", default=str(OUT))
    ap.add_argument("--config", default=str(CONFIG), help="N64Recomp config (for stubs/ignored/instruction patches)")
    args = ap.parse_args(argv)
    rom = Path(args.rom).read_bytes()
    syms = tomllib.loads(Path(args.syms).read_text())
    config = Path(args.config)
    reimpl, ignored = runtime_name_lists()
    cfg = tomllib.loads(config.read_text()) if config.exists() else {}
    handled = reimpl | ignored | set(cfg.get("patches", {}).get("stubs", [])) | set(cfg.get("patches", {}).get("ignored", []))

    patched = {(ip["func"], ip["vram"]) for ip in cfg.get("patches", {}).get("instruction", [])}

    hits = 0
    for sec in syms["section"]:
        for f in sec["functions"]:
            if f["name"] in handled:
                continue
            notes = []
            hw_regs = {}  # register -> address of the lui that loaded a KSEG1 upper half
            for a in range(f["vram"], f["vram"] + f["size"], 4):
                o = sec["rom"] + (a - sec["vram"])
                w = int.from_bytes(rom[o:o + 4], "big")
                if (f["name"], a) in patched:
                    continue
                ins = rabbitizer.Instruction(w, vram=a)
                op = ins.getOpcodeName()
                if op in HW_IDS:
                    notes.append(f"{a:08X} {ins.disassemble()}")
                    continue
                rs, rt = (w >> 21) & 31, (w >> 16) & 31
                if op == "lui":
                    if 0xA000 <= (w & 0xFFFF) <= 0xBFFF:
                        hw_regs[rt] = a
                    else:
                        hw_regs.pop(rt, None)
                    continue
                # A load/store based on that register is a real hardware access; a float constant
                # (mtc1) or a value passed as an argument (e.g. a PI device address) is not.
                if (ins.doesLoad() or ins.doesStore()) and rs in hw_regs:
                    notes.append(f"{a:08X} {ins.disassemble()}   (base from lui at {hw_regs[rs]:08X})")
                elif ins.modifiesRt() and rt in hw_regs and op not in ("ori", "addiu"):
                    hw_regs.pop(rt, None)
                elif ins.modifiesRd() and ((w >> 11) & 31) in hw_regs:
                    hw_regs.pop((w >> 11) & 31, None)  # e.g. addu v0, a1, a0 replaces the lui'd value
            if notes:
                hits += 1
                print(f"{sec['name']:6} {f['name']} (0x{f['vram']:08X}, 0x{f['size']:X})")
                for n in notes[:6]:
                    print(f"         {n}")
                if len(notes) > 6:
                    print(f"         ... {len(notes) - 6} more")
    print(f"\n{hits} function(s) with direct hardware access that the runtime does not handle.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
