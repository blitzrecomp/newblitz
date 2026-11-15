#!/usr/bin/env python3
"""32-bit registers post-pass over N64Recomp output (the PSP port, docs/PSP_PORT.md section 3.2).

The generated C keeps every MIPS register as a 64-bit sign extension (recomp.h's `gpr`). With
RECOMP_GPR32 the runtime's recomp.h (platform/n64recomp-patches/0001-gpr32-recomp-h.patch) makes a
gpr the low 32 bits instead. That is only sound because the game uses no 64-bit integer
instruction, so this pass checks that first and fails the build on any. It then makes the few
changes the header can't:

  1. Tripwire: any `// 0x...: <mnemonic>` comment naming a 64-bit op (d* integer ops, ld/sd/ldl/ldr/
     sdl/sdr/lwu/lld/scd, dmfc1/dmtc1, the .l FPU conversions) fails with its function and vram.
     A second check refuses the generator's 64-bit macros (DMULT, DDIV, do_ldl, ...) in the C.
  2. `div`: the generator's 64-bit division
         lo = S32(S64(S32(A)) / S64(S32(B))); hi = S32(S64(S32(A)) % S64(S32(B)));
     becomes { uint32_t _l, _h; DIV32(A, B, &_l, &_h); lo = _l; hi = _h; } (one 32-bit divide, the
     VR4300's results for x/0 and INT_MIN/-1). The number of sites must be the flavour's expected
     count exactly; a different number is an error (the game's code changed: check, then update).
  3. `uint64_t hi = 0, lo = 0, result = 0;` -> `uint32_t hi = 0, lo = 0; uint64_t result = 0;`
     (result holds mult/multu's 64-bit product).
  4. `#define RECOMP_GPR32 1` first in each file (CMake defines it for every TU as well).
  5. RECOMP_RESTRICT on each function's rdram and ctx (recomp.h defines it empty for now).
  6. <out>/gpr32_report.txt: div/divu sites and their zero checks, mult sites, double-FP ops per
     function, LOOKUP_FUNC sites and the inline hook lines.

Usage:
  gpr32.py --in build/RecompiledFuncs --out build/psp/classic/RecompiledFuncs --flavour classic
  gpr32.py --in build/engine32/RecompiledFuncs --out build/psp/modern/RecompiledFuncs --flavour modern
  gpr32.py --selftest      runs the pass on a seeded fake `dsll`, which must fail the build; exits 0
                           when it did (and when a clean sample passes), 1 otherwise
"""
import argparse
import collections
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

# div sites per flavour (Classic: build/RecompiledFuncs; Modern: build/engine32/RecompiledFuncs). Counted 2026-11-03.
EXPECTED_DIV = {'classic': 74, 'modern': 74}

FUNC_START = re.compile(r'^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{\s*$')
FUNC_END = re.compile(r'^;\}\s*$')
INSN_COMMENT = re.compile(r'^\s*// 0x([0-9A-F]{8}): (\S+)\s*(.*)$')
LOCALS = '    uint64_t hi = 0, lo = 0, result = 0;'
LOCALS_32 = '    uint32_t hi = 0, lo = 0; uint64_t result = 0;'
DIV = re.compile(r'^(\s*)lo = S32\(S64\(S32\((?P<a>[^()]+)\)\) / S64\(S32\((?P<b>[^()]+)\)\)\); '
                 r'hi = S32\(S64\(S32\((?P=a)\)\) % S64\(S32\((?P=b)\)\)\);\s*$')
DIVU = re.compile(r'^\s*lo = S32\(U32\((?P<a>[^()]+)\) / U32\((?P<b>[^()]+)\)\); '
                  r'hi = S32\(U32\((?P=a)\) % U32\((?P=b)\)\);\s*$')
MULT = re.compile(r'^\s*result = (S64\(S32|U64\(U32)\(')
PLAIN_CALL = re.compile(r'^\s*\w+\(rdram, ctx\);\s*$')
LOOKUP = re.compile(r'LOOKUP_FUNC\(([^)]*)\)')
DEFINE = '#ifndef RECOMP_GPR32\n#define RECOMP_GPR32 1\n#endif\n'
GUARD = ('#ifndef DIV32\n#error "32-bit register code (tools/psp/gpr32.py) needs the RECOMP_GPR32 recomp.h '
         '(platform/n64recomp-patches)"\n#endif\n')

# 64-bit integer mnemonics as rabbitizer prints them. 'div', 'divu', 'div.s' and 'div.d' are 32-bit or FPU.
D_INT = re.compile(r'^d(add|addi|addiu|addu|sub|subu|mult|multu|div|divu|sll|sll32|sllv|srl|srl32|srlv|sra|sra32|srav|'
                   r'neg|negu|mfc0|mtc0|mfc1|mtc1|mfc2|mtc2)$')
MEM64 = {'ld', 'sd', 'ldl', 'ldr', 'sdl', 'sdr', 'lwu', 'lld', 'scd'}
FPU_L = re.compile(r'^(cvt\.[sdw]\.l|cvt\.l\.[sdw]|(trunc|round|ceil|floor)\.l\.[sd])$')
# The generator's 64-bit macros: a second line of defence should a mnemonic ever be spelled differently.
C64 = re.compile(r'\b(DMULTU?|DDIVU?|do_ldl|do_ldr|do_sdl|do_sdr|MEM_WU|CVT_[DS]_L|CVT_L_[DS]|TRUNC_L_[DS]|llroundf?)\b'
                 r'|^\s*ctx->r\d+ = LD\(|\bSD\(ctx->r\d+|S64\((ceil|floor)f?\(')

DOUBLE_MN = re.compile(r'(\.d$|^cvt\.d\.|^c\.\w+\.d$|^ldc1$|^sdc1$)')


def is_64bit(mn):
    return bool(D_INT.match(mn)) or mn in MEM64 or bool(FPU_L.match(mn))


class Tripwire(Exception):
    pass


def transform(src, out, expected_div, shown_out=None):
    """src -> out (a fresh directory). Returns the report text. Raises Tripwire or ValueError."""
    names = sorted(os.listdir(src))
    funcs_files = [n for n in names if re.match(r'^funcs_\d+\.c$', n)]
    if not funcs_files:
        raise ValueError(f'no funcs_*.c in {src}')
    hits = []          # (function, vram, mnemonic, file:line)
    c_hits = []
    n_funcs = n_locals = n_sigs = 0
    div_sites, divu_sites = [], []   # (function, vram, a, b, guarded_zero, guarded_overflow)
    mult = collections.Counter()
    double_ops = collections.defaultdict(collections.Counter)   # function -> mnemonic -> unique sites
    lookups = []       # (function, vram, expr)
    hook_lines = []    # (function, text)
    os.makedirs(out)

    for name in funcs_files:
        lines = open(os.path.join(src, name)).read().split('\n')
        res = [DEFINE.rstrip('\n')]
        func, vram, seen = None, None, set()
        i = 0
        while i < len(lines):
            line = lines[i]
            m = FUNC_START.match(line)
            if m:
                func, vram, seen = m.group(1), None, set()
                n_funcs += 1
                n_sigs += 1
                res.append(f'RECOMP_FUNC void {func}(uint8_t* RECOMP_RESTRICT rdram, recomp_context* RECOMP_RESTRICT ctx) {{')
                i += 1
                continue
            if FUNC_END.match(line):
                func = None
            c = INSN_COMMENT.match(line)
            if c:
                v, mn = c.group(1), c.group(2)
                if func and vram is None:
                    vram = v
                if is_64bit(mn):
                    hits.append((func, v, mn + (' ' + c.group(3) if c.group(3) else ''), f'{name}:{i + 1}'))
                if func and (v, mn) not in seen:
                    seen.add((v, mn))
                    if DOUBLE_MN.search(mn):
                        double_ops[func][mn] += 1
                res.append(line)
                i += 1
                continue
            s = line.strip()
            if s and not s.startswith('//') and C64.search(line):
                c_hits.append((func, f'{name}:{i + 1}', s[:120]))
            if line == LOCALS:
                n_locals += 1
                res.append(LOCALS_32)
                i += 1
                continue
            d = DIV.match(line)
            if d:
                g0, g1 = guards(lines, i)
                div_sites.append((func, last_vram(lines, i), d.group('a'), d.group('b'), g0, g1))
                res.append(f"{d.group(1)}{{ uint32_t _l, _h; DIV32({d.group('a')}, {d.group('b')}, &_l, &_h); lo = _l; hi = _h; }}")
                i += 1
                continue
            if ' / S64(' in line or ' % S64(' in line:
                raise ValueError(f'{name}:{i + 1}: a 64-bit division the div rewrite does not recognise: {s[:120]}')
            du = DIVU.match(line)
            if du:
                g0, _ = guards(lines, i)
                divu_sites.append((func, last_vram(lines, i), du.group('a'), du.group('b'), g0, None))
            mm = MULT.match(line)
            if mm:
                mult['mult' if mm.group(1).startswith('S') else 'multu'] += 1
            for lk in LOOKUP.finditer(line):
                lookups.append((func, last_vram(lines, i), lk.group(1)))
            if func and 'blitz_' in line and not s.startswith('//') and not PLAIN_CALL.match(line):
                hook_lines.append((func, s))
            res.append(line)
            i += 1
        with open(os.path.join(out, name), 'w') as f:
            f.write('\n'.join(res[:1]) + '\n' + '\n'.join(insert_guard(res[1:])))

    if hits or c_hits:
        msg = ['gpr32: 64-bit integer operations in the recompiled code; 32-bit registers cannot run them:']
        for func, v, mn, where in hits:
            msg.append(f'  {func or "?"} 0x{v}: {mn}   ({where})')
        for func, where, s in c_hits:
            msg.append(f'  {func or "?"}: 64-bit macro in C: {s}   ({where})')
        raise Tripwire('\n'.join(msg))
    if n_locals != n_funcs or n_sigs != n_funcs:
        raise ValueError(f'{n_funcs} functions but {n_locals} hi/lo declarations and {n_sigs} signatures rewritten '
                         '(the generator\'s output format changed?)')
    if len(div_sites) != expected_div:
        raise ValueError(f'{len(div_sites)} div sites, expected exactly {expected_div}. The game\'s code changed: '
                         'check the new sites in the report, then update EXPECTED_DIV in tools/psp/gpr32.py')

    for name in names:   # funcs.h, lookup.cpp, recomp_overlays.inl, ...: the define ahead of their recomp.h
        if name in funcs_files or os.path.isdir(os.path.join(src, name)):
            continue
        text = open(os.path.join(src, name)).read()
        if name.endswith(('.c', '.cpp', '.h', '.inl')) and '#include "recomp.h"' in text:
            text = DEFINE + text
        with open(os.path.join(out, name), 'w') as f:
            f.write(text)

    r = [f'gpr32 report: {src} -> {shown_out or out}', '',
         f'functions: {n_funcs}; 64-bit integer ops: 0 (tripwire passed)',
         f'div sites: {len(div_sites)} (expected {expected_div}), rewritten to DIV32',
         f'  with the compiler\'s zero check (break 7) after them: {sum(1 for d in div_sites if d[4])}; '
         f'with the INT_MIN/-1 check (break 6): {sum(1 for d in div_sites if d[5])}',
         f'divu sites: {len(divu_sites)} (32-bit already, unchanged); with the zero check: {sum(1 for d in divu_sites if d[4])}',
         f'mult: {mult["mult"]}, multu: {mult["multu"]} (64-bit product of 32-bit operands in result; lo/hi 32-bit)',
         f'LOOKUP_FUNC sites: {len(lookups)} ({sum(1 for l in lookups if l[2].startswith("ctx->"))} through a register)',
         f'inline hook lines (blitz_* statements other than a plain call): {len(hook_lines)}',
         f'functions with double-precision FPU ops: {len(double_ops)}; unique double op sites: '
         f'{sum(sum(c.values()) for c in double_ops.values())}', '']
    r.append('div sites (function, vram, dividend, divisor, zero check, overflow check):')
    for func, v, a, b, g0, g1 in div_sites:
        r.append(f'  {func} 0x{v} {a} / {b}  zero={"yes" if g0 else "NO"} overflow={"yes" if g1 else "NO"}')
    r.append('divu sites:')
    for func, v, a, b, g0, _ in divu_sites:
        r.append(f'  {func} 0x{v} {a} / {b}  zero={"yes" if g0 else "NO"}')
    r.append('LOOKUP_FUNC sites (function, vram of the nearest instruction, target):')
    for func, v, e in lookups:
        r.append(f'  {func} 0x{v} {e}')
    r.append('double-precision FPU ops per function (unique sites):')
    for func, cnt in sorted(double_ops.items(), key=lambda kv: (-sum(kv[1].values()), kv[0])):
        r.append(f'  {sum(cnt.values()):4d} {func}: ' + ', '.join(f'{k} {n}' for k, n in sorted(cnt.items())))
    r.append('inline hook lines:')
    for func, s in hook_lines:
        r.append(f'  {func}: {s}')
    return '\n'.join(r) + '\n'


def insert_guard(lines):
    """The DIV32 guard right after the file's last #include at the top."""
    last = -1
    for k, l in enumerate(lines[:20]):
        if l.startswith('#include'):
            last = k
    return lines[:last + 1] + GUARD.rstrip('\n').split('\n') + lines[last + 1:]


def last_vram(lines, i):
    for k in range(i, max(-1, i - 40), -1):
        c = INSN_COMMENT.match(lines[k])
        if c:
            return c.group(1)
    return '????????'


def guards(lines, i):
    """Whether the compiler's `break 7` (divide by zero) and `break 6` (overflow) follow a division."""
    g7 = g6 = False
    n = 0
    for k in range(i + 1, min(len(lines), i + 80)):
        c = INSN_COMMENT.match(lines[k])
        if not c:
            continue
        n += 1
        if c.group(2) == 'break':
            g7 |= c.group(3).strip() == '7'
            g6 |= c.group(3).strip() == '6'
        if c.group(2) in ('div', 'divu', 'jr') or n > 24:
            break
    return g7, g6


def run(src, out, expected_div, report_name='gpr32_report.txt'):
    parent = os.path.dirname(os.path.abspath(out))
    os.makedirs(parent, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='.gpr32_', dir=parent)
    try:
        work = os.path.join(tmp, 'out')
        report = transform(src, work, expected_div, out)
        with open(os.path.join(work, report_name), 'w') as f:
            f.write(report)
        if os.path.exists(out):
            shutil.rmtree(out)
        os.rename(work, out)
        return report
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


SAMPLE = '''#include "recomp.h"
#include "blitz_hooks.h"
#include "funcs.h"

RECOMP_FUNC void func_80000010(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x80000010: div         $zero, $a0, $a1
    lo = S32(S64(S32(ctx->r4)) / S64(S32(ctx->r5))); hi = S32(S64(S32(ctx->r4)) % S64(S32(ctx->r5)));
    // 0x80000014: bne         $a1, $zero, L_80000020
    if (ctx->r5 != 0) {
        // 0x80000018: nop
            goto L_80000020;
    }
    // 0x80000018: nop

    // 0x8000001C: break       7
    do_break(2147483676);
L_80000020:
    // 0x80000020: mflo        $v0
    ctx->r2 = lo;
@SEED@    // 0x80000024: jr          $ra
    // 0x80000028: nop
    return;
;}
'''


def selftest():
    ok = True
    with tempfile.TemporaryDirectory() as t:
        src = os.path.join(t, 'in')
        os.makedirs(src)
        seed = '    // 0x80000024: dsll        $v0, $v0, 2\n    ctx->r2 = ctx->r2 << 2;\n'
        with open(os.path.join(src, 'funcs_0.c'), 'w') as f:
            f.write(SAMPLE.replace('@SEED@', seed))
        try:
            run(src, os.path.join(t, 'out1'), 1)
            print('selftest FAILED: the seeded dsll was not caught')
            ok = False
        except Tripwire as e:
            caught = 'func_80000010 0x80000024: dsll' in str(e)
            print(('selftest: the seeded dsll failed the build as it should:\n' if caught else
                   'selftest FAILED: tripwire fired without naming the seed:\n') + str(e))
            ok &= caught
        with open(os.path.join(src, 'funcs_0.c'), 'w') as f:
            f.write(SAMPLE.replace('@SEED@', ''))
        try:
            run(src, os.path.join(t, 'out2'), 2)
            print('selftest FAILED: a wrong div count was accepted')
            ok = False
        except ValueError as e:
            print(f'selftest: a wrong div count is refused: {e}')
        report = run(src, os.path.join(t, 'out3'), 1)
        text = open(os.path.join(t, 'out3', 'funcs_0.c')).read()
        for want in ('#define RECOMP_GPR32 1', LOCALS_32, 'DIV32(ctx->r4, ctx->r5, &_l, &_h); lo = _l; hi = _h;',
                     'uint8_t* RECOMP_RESTRICT rdram, recomp_context* RECOMP_RESTRICT ctx', '#ifndef DIV32'):
            if want not in text:
                print(f'selftest FAILED: output lacks {want!r}')
                ok = False
        if 'zero=yes overflow=NO' not in report:
            print('selftest FAILED: the zero check was not found')
            ok = False
    print('selftest ok' if ok else 'selftest FAILED')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--in', dest='src', default=os.path.join(ROOT, 'build/RecompiledFuncs'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/psp/classic/RecompiledFuncs'))
    ap.add_argument('--flavour', choices=sorted(EXPECTED_DIV), default='classic')
    ap.add_argument('--expect-div', type=int, default=None, help='override the flavour\'s expected div count')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    expected = a.expect_div if a.expect_div is not None else EXPECTED_DIV[a.flavour]
    try:
        report = run(a.src, a.out, expected)
    except (Tripwire, ValueError) as e:
        print(e, file=sys.stderr)
        return 1
    print('\n'.join(report.split('\n')[:10]).rstrip())
    return 0


if __name__ == '__main__':
    sys.exit(main())
