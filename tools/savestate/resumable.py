#!/usr/bin/env python3
"""Resumable-function post-pass over N64Recomp output (save states, design A2 in docs/SAVE_STATES.md).

Every Blitz process is a host thread parked inside process_sleep -> blitz_osStopThread, and the
only record of how it got there is the host call stack. This pass rewrites the generated C so that
the call chain is also kept on a small per-thread *shadow stack* of call-site ids, and so that a
fresh host thread can walk that chain back down:

  - S = every recompiled function that can (transitively) reach a sink (process_sleep's
    blitz_osStopThread call; optionally osRecvMesg_recomp for the fixed threads).
  - Each call in an S function to an S callee, an indirect call (LOOKUP_FUNC), or a sink gets a
    globally unique site id. Before the call the function does SS_PUSH(site[, target]); a label
    ss_r_<id> sits right before the call statement (after the delay slot, which already ran);
    after the call SS_POP().
  - At entry, if the thread is resuming (SS_RESUMING), the function reads the next site id and
    jumps to that label. Registers come from the restored recomp_context, so nothing between
    the function entry and the call needs to run. Indirect sites call the recorded target.
  - A sink site is the bottom of the chain: 'after' (process_sleep: continue as if
    blitz_osStopThread returned) or 'redo' (re-issue the blocking call, e.g. osRecvMesg).

Usage:
  resumable.py [--in build/RecompiledFuncs] [--out build/savestate/RecompiledFuncs]
               [--sinks recv] [--indirect conservative|direct] [--report FILE] [--no-emit]

Outputs, besides the transformed funcs_*.c (non-S functions are copied verbatim):
  <out>/ss_sites.json   site id -> function, call vram, callee/target expr, kind
  <out>/ss_sites.h      SS_SITE_COUNT and a table usable by the runtime
"""
import argparse
import collections
import json
import os
import re
import shutil
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

FUNC_START = re.compile(r'^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{\s*$')
FUNC_END = re.compile(r'^;\}\s*$')
DIRECT_CALL = re.compile(r'^(\s*)(\w+)\(rdram, ctx\);\s*$')
INDIRECT_CALL = re.compile(r'^(\s*)LOOKUP_FUNC\((.*)\)\(rdram, ctx\);\s*$')
AFTER_GOTO = re.compile(r'^\s*goto (after_\d+);\s*$')
INSN_COMMENT = re.compile(r'^\s*// 0x([0-9A-F]{8}): (\S+)\s*(.*)$')
LABEL = re.compile(r'^\s*(L_[0-9A-F]{8}|after_\d+|ss_r_\d+):\s*$')
GOTO = re.compile(r'^\s*goto (\w+);\s*$')
LOCAL_DECL = re.compile(r'^\s*(?:gpr|uint64_t|int|uint32_t|int32_t|float|double)\s+(.+);\s*$')
VLA = re.compile(r'^\s*\w[\w\s\*]*\s+\w+\s*\[[^\]0-9][^\]]*\]\s*;')

# Sinks: (function containing the call, callee) -> resume mode.
SINKS_SLEEP = {('process_sleep', 'blitz_osStopThread'): 'after'}
SINK_RECV_CALLEE = 'osRecvMesg_recomp'


class Call:
    __slots__ = ('line', 'indent', 'kind', 'callee', 'follow', 'after', 'vram', 'insn', 'site', 'mode')

    def __init__(self, line, indent, kind, callee, follow, after):
        self.line, self.indent, self.kind, self.callee = line, indent, kind, callee
        self.follow, self.after = follow, after
        self.vram = self.insn = None
        self.site = None
        self.mode = None  # 'call' | 'after' | 'redo'


class Func:
    def __init__(self, name, fname, start):
        self.name, self.file, self.start = name, fname, start
        self.end = None
        self.vram = None
        self.calls = []
        self.hooks = []      # (line, name) host hook calls (not followed by goto after_/return)
        self.locals = set()  # C locals other than the standard ones
        self.has_vla = False


def parse(src_dir):
    files = {}
    funcs = {}
    for fn in sorted(os.listdir(src_dir), key=lambda s: (len(s), s)):
        if not (fn.startswith('funcs_') and fn.endswith('.c')):
            continue
        lines = open(os.path.join(src_dir, fn)).read().split('\n')
        files[fn] = lines
        cur = None
        for i, l in enumerate(lines):
            m = FUNC_START.match(l)
            if m:
                cur = Func(m.group(1), fn, i)
                funcs[cur.name] = cur
                continue
            if cur is None:
                continue
            if FUNC_END.match(l):
                cur.end = i
                cur = None
                continue
            if cur.vram is None:
                mi = INSN_COMMENT.match(l)
                if mi:
                    cur.vram = int(mi.group(1), 16)
            md = LOCAL_DECL.match(l)
            if md and '(' not in md.group(1).split('=')[0]:
                for part in md.group(1).split(','):
                    nm = part.split('=')[0].strip().lstrip('*')
                    if re.match(r'^\w+$', nm):
                        cur.locals.add(nm)
            if VLA.match(l):
                cur.has_vla = True
            m = INDIRECT_CALL.match(l)
            kind = 'indirect'
            if not m:
                m = DIRECT_CALL.match(l)
                kind = 'direct'
            if not m:
                continue
            nxt = lines[i + 1] if i + 1 < len(lines) else ''
            ga = AFTER_GOTO.match(nxt)
            if ga:
                follow, after = 'after', ga.group(1)
            elif nxt.strip() == 'return;':
                follow, after = 'return', None
            else:
                cur.hooks.append((i, m.group(2)))
                continue
            c = Call(i, m.group(1), kind, m.group(2), follow, after)
            # The branch/jump instruction this call came from: the nearest jal/jalr/j/jr comment above.
            for k in range(i - 1, max(cur.start, i - 8), -1):
                mi = INSN_COMMENT.match(lines[k])
                if mi and mi.group(2) in ('jal', 'jalr', 'j', 'jr', 'bal', 'bgezal', 'bltzal'):
                    c.vram, c.insn = int(mi.group(1), 16), mi.group(2)
                    break
            cur.calls.append(c)
    return files, funcs


def compute_set(funcs, sink_pairs, sink_callees, indirect_may_sleep):
    """Functions that can reach a sink. sink_pairs: {(func, callee)}; sink_callees: callees that are
    sinks wherever they are called (e.g. osRecvMesg_recomp)."""
    S = set()
    for f in funcs.values():
        for c in f.calls:
            if c.kind == 'direct' and ((f.name, c.callee) in sink_pairs or c.callee in sink_callees):
                S.add(f.name)
    rev = collections.defaultdict(set)
    has_ind = set()
    for f in funcs.values():
        for c in f.calls:
            if c.kind == 'direct' and c.callee in funcs:
                rev[c.callee].add(f.name)
            elif c.kind == 'indirect':
                has_ind.add(f.name)
    work = list(S)
    if indirect_may_sleep:
        for n in has_ind:
            if n not in S:
                S.add(n)
                work.append(n)
    while work:
        x = work.pop()
        for p in rev[x]:
            if p not in S:
                S.add(p)
                work.append(p)
    return S


# ---------------------------------------------------------------- liveness of C locals across calls

READ_SKIP = re.compile(r'^\s*//')


def liveness_issues(lines, f, sites, locals_):
    """For each instrumented call, is any C local (hi, lo, result, c1cs, jr_addend_*) read after the
    call before being written, on some path? Those values would be lost on resume (they're 0 at
    function entry). Small CFG walk over the generated C: labels, gotos, if-blocks, switch."""
    body = list(range(f.start + 1, f.end))
    label_at = {}
    for i in body:
        m = LABEL.match(lines[i])
        if m:
            label_at[m.group(1)] = i
    # matching closing brace for each 'if (...) {' line
    close = {}
    stack = []
    for i in body:
        s = lines[i]
        if READ_SKIP.match(s):
            continue
        opens = s.count('{')
        closes = s.count('}')
        for _ in range(opens):
            stack.append(i)
        for _ in range(closes):
            if stack:
                close[stack.pop()] = i
    word = {v: re.compile(r'\b%s\b' % re.escape(v)) for v in locals_}
    out = []
    for c in sites:
        start = label_at.get(c.after) if c.follow == 'after' else None
        if start is None:
            continue  # tail call: nothing runs after it in this function
        live = set()
        seen = set()
        todo = [(start + 1, frozenset(locals_))]
        while todo:
            i, pending = todo.pop()
            while i < f.end and pending:
                if (i, pending) in seen:
                    break
                seen.add((i, pending))
                s = lines[i]
                st = s.strip()
                if not st or READ_SKIP.match(s) or LABEL.match(s):
                    i += 1
                    continue
                if st == 'return;':
                    break
                newp = set(pending)
                # a line can hold several statements ("result = ...; lo = ...; hi = ...;")
                stmts = [st] if st.startswith(('if', 'switch', '}', 'case')) else [x for x in st.split(';') if x.strip()]
                for stmt in stmts:
                    stmt = stmt.strip()
                    lhs, rhs = None, stmt
                    mm = re.match(r'^(?:gpr\s+|uint64_t\s+|int\s+)?(\w+)\s*=\s*(.*)$', stmt)
                    if mm and not stmt.startswith('if'):
                        lhs, rhs = mm.group(1), mm.group(2)
                    for v in list(newp):
                        if word[v].search(rhs):
                            live.add(v)
                            newp.discard(v)
                    if lhs in newp:
                        newp.discard(lhs)
                pending = frozenset(newp)
                g = GOTO.match(s)
                if g:
                    tgt = label_at.get(g.group(1))
                    if tgt is not None:
                        todo.append((tgt, pending))
                    break
                if st.startswith('if (') and st.endswith('{') and i in close:
                    todo.append((close[i] + 1, pending))
                if st.startswith('switch (') and i in close:
                    for k in range(i + 1, close[i]):
                        mg = re.search(r'goto (\w+);', lines[k])
                        if mg and mg.group(1) in label_at:
                            todo.append((label_at[mg.group(1)], pending))
                    break
                i += 1
        if live:
            out.append((c, sorted(live)))
    return out


# ---------------------------------------------------------------- address-taken functions (indirect targets)

def parse_sections(src_dir):
    """Section table and per-function rom ranges from recomp_overlays.inl."""
    txt = open(os.path.join(src_dir, 'recomp_overlays.inl')).read()
    secs = []
    for m in re.finditer(r'\{ \.rom_addr = 0x([0-9A-F]+), \.ram_addr = 0x([0-9A-F]+), \.size = 0x([0-9A-F]+), \.funcs = (\w+)', txt):
        secs.append(dict(rom=int(m.group(1), 16), ram=int(m.group(2), 16), size=int(m.group(3), 16), table=m.group(4)))
    code = []  # (rom_start, rom_end)
    func_off = {}
    for sec in secs:
        mt = re.search(r'static FuncEntry %s\[\] = \{(.*?)\n\};' % sec['table'], txt, re.S)
        if not mt:
            continue
        for m in re.finditer(r'\.func = (\w+), \.offset = 0x([0-9A-F]+), \.rom_size = 0x([0-9A-F]+)', mt.group(1)):
            off, sz = int(m.group(2), 16), int(m.group(3), 16)
            code.append((sec['rom'] + off, sec['rom'] + off + sz))
            func_off[m.group(1)] = (sec, off)
    return secs, code, func_off


def address_taken(files, funcs, src_dir, rom_path):
    by_vram = collections.defaultdict(list)
    for f in funcs.values():
        if f.vram is not None:
            by_vram[f.vram].append(f.name)
    code_refs = collections.defaultdict(set)  # target name -> referencing functions
    lui = re.compile(r'^\s*ctx->r(\d+) = S32\(0X([0-9A-F]+) << 16\);')
    addi = re.compile(r'^\s*ctx->r(\d+) = ADD32\(ctx->r(\d+), (-?)0X([0-9A-F]+)\);')
    ori = re.compile(r'^\s*ctx->r(\d+) = ctx->r(\d+) \| 0X([0-9A-F]+);')
    assign = re.compile(r'^\s*ctx->r(\d+) = ')
    for f in funcs.values():
        lines = files[f.file]
        hi = {}
        for i in range(f.start + 1, f.end):
            s = lines[i]
            if LABEL.match(s):
                pass  # keep values across labels (approximation)
            m = lui.match(s)
            if m:
                hi[m.group(1)] = int(m.group(2), 16) << 16
                continue
            m = addi.match(s) or ori.match(s)
            if m and m.group(2) in hi:
                if m.re is addi:
                    v = (hi[m.group(2)] + (-1 if m.group(3) else 1) * int(m.group(4), 16)) & 0xFFFFFFFF
                else:
                    v = hi[m.group(2)] | int(m.group(3), 16)
                if v in by_vram:
                    for n in by_vram[v]:
                        code_refs[n].add(f.name)
                hi.pop(m.group(1), None)
                continue
            m = assign.match(s)
            if m:
                hi.pop(m.group(1), None)
    data_refs = collections.defaultdict(set)  # target name -> rom offsets of the pointer words
    if rom_path and os.path.exists(rom_path):
        secs, code, _ = parse_sections(src_dir)
        rom = open(rom_path, 'rb').read()
        code.sort()
        in_code = bytearray(len(rom) // 4 + 1)
        for a, b in code:
            for w in range(a // 4, (b + 3) // 4):
                if w < len(in_code):
                    in_code[w] = 1
        for sec in secs:
            for off in range(sec['rom'], min(sec['rom'] + sec['size'], len(rom)) - 3, 4):
                if in_code[off // 4]:
                    continue
                v = struct.unpack_from('>I', rom, off)[0]
                if v in by_vram:
                    for n in by_vram[v]:
                        data_refs[n].add(off)
    return code_refs, data_refs


# ---------------------------------------------------------------- transform

def assign_sites(funcs, S, sink_pairs, sink_callees, sink_modes):
    sites = []
    for name in sorted(S, key=lambda n: (funcs[n].file, funcs[n].start)):
        f = funcs[name]
        for c in f.calls:
            if c.kind == 'indirect':
                c.mode = 'call'
            elif (f.name, c.callee) in sink_pairs:
                c.mode = sink_modes[(f.name, c.callee)]
            elif c.callee in sink_callees:
                c.mode = 'redo'
            elif c.callee in S:
                c.mode = 'call'
            else:
                continue
            c.site = len(sites) + 1  # 0 = "no site"
            sites.append((f, c))
    return sites


def transform_func(lines, f, out):
    inst = [c for c in f.calls if c.site is not None]
    if not inst:
        out.extend(lines[f.start:f.end + 1])
        return
    by_line = {c.line: c for c in inst}
    has_ind = any(c.kind == 'indirect' for c in inst)
    # Locate the end of the standard local declarations ("int c1cs = 0;").
    decl_end = f.start
    for k in range(f.start + 1, min(f.end, f.start + 6)):
        if lines[k].strip().startswith('int c1cs'):
            decl_end = k
            break
    out.extend(lines[f.start:decl_end + 1])
    # A teamplus ENTRY hook (tools/teamplus.py: "if (blitz_adv) { host(rdram, ctx); return; }") goes before the resume
    # switch: in an Advanced Season the host driver replaces the function, so a restored thread must reach the host
    # (adv_engine.cpp's blitz_adv_week_driver resumes the match itself) and not the vanilla code's own call sites.
    body_from = decl_end + 1
    if body_from <= f.end and re.match(r'^\s*if \(blitz_adv\) \{.*; return; \}\s*$', lines[body_from]):
        out.append(lines[body_from])
        body_from += 1
    if has_ind:
        out.append('    uint32_t ss_tgt = 0;')
    out.append('    if (SS_RESUMING(ctx)) {')
    out.append('        switch (SS_RESUME_SITE(ctx)) {')
    for c in inst:
        if c.mode == 'after':
            # bottom of the chain: continue as if the blocking call had returned
            if c.follow == 'after':
                out.append('            case %d: SS_RESUME_DONE(ctx); SS_POP(ctx); goto %s;' % (c.site, c.after))
            else:
                out.append('            case %d: SS_RESUME_DONE(ctx); SS_POP(ctx); return;' % c.site)
        elif c.mode == 'redo':
            out.append('            case %d: SS_RESUME_DONE(ctx); goto ss_r_%d;' % (c.site, c.site))
        elif c.kind == 'indirect':
            out.append('            case %d: ss_tgt = SS_RESUME_TARGET(ctx); goto ss_r_%d;' % (c.site, c.site))
        else:
            out.append('            case %d: goto ss_r_%d;' % (c.site, c.site))
    out.append('            default: SS_RESUME_FAIL(ctx, 0x%08Xu); break;' % (f.vram or 0))
    out.append('        }')
    out.append('    }')
    for i in range(body_from, f.end + 1):
        c = by_line.get(i)
        if c is None:
            out.append(lines[i])
            continue
        ind = c.indent
        if c.kind == 'indirect':
            out.append('%sss_tgt = (uint32_t)(%s);' % (ind, c.callee))
            out.append('%sSS_PUSH_T(ctx, %d, ss_tgt);' % (ind, c.site))
            out.append('ss_r_%d:' % c.site)
            out.append('%sSS_INDIRECT(ctx, ss_tgt)(rdram, ctx);' % ind)
        else:
            out.append('%sSS_PUSH(ctx, %d);' % (ind, c.site))
            out.append('ss_r_%d:' % c.site)
            out.append(lines[i])
        out.append('%sSS_POP(ctx);' % ind)


def emit(files, funcs, sites, in_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    infile = collections.defaultdict(list)
    for f in funcs.values():
        infile[f.file].append(f)
    for fn, lines in files.items():
        fl = sorted(infile[fn], key=lambda f: f.start)
        out = []
        pos = 0
        for f in fl:
            out.extend(lines[pos:f.start])
            transform_func(lines, f, out)
            pos = f.end + 1
        out.extend(lines[pos:])
        # make the API visible: after the first #include line
        for k, l in enumerate(out):
            if l.startswith('#include "funcs.h"'):
                out.insert(k + 1, '#include "ss_runtime.h"')
                break
        else:
            out.insert(0, '#include "ss_runtime.h"')
        with open(os.path.join(out_dir, fn), 'w') as fh:
            fh.write('\n'.join(out))
    for extra in os.listdir(in_dir):
        if not (extra.startswith('funcs_') and extra.endswith('.c')):
            shutil.copy(os.path.join(in_dir, extra), os.path.join(out_dir, extra))
    shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ss_runtime.h'), os.path.join(out_dir, 'ss_runtime.h'))
    table = []
    for f, c in sites:
        cv = funcs[c.callee].vram if (c.kind == 'direct' and c.callee in funcs) else 0
        table.append(dict(site=c.site, func=f.name, func_vram='0x%08X' % (f.vram or 0),
                          call_vram='0x%08X' % (c.vram or 0), callee_vram='0x%08X' % (cv or 0), insn=c.insn, kind=c.kind,
                          callee=c.callee, mode=c.mode, tail=(c.follow == 'return')))
    with open(os.path.join(out_dir, 'ss_sites.json'), 'w') as fh:
        json.dump(table, fh, indent=1)
    with open(os.path.join(out_dir, 'ss_sites.h'), 'w') as fh:
        fh.write('// Generated by tools/savestate/resumable.py\n#pragma once\n#define SS_SITE_COUNT %d\n' % len(sites))
        fh.write('// site -> {function vram, call instruction vram, callee vram (0: indirect or sink)}.\n'
                 '// A snapshot stores these so a loader can check (or remap) site ids against another\n'
                 '// build; the callee column lets the runtime check that a thread\'s chain is contiguous\n'
                 '// (entry i+1 runs in the function site i calls; for indirect sites, in its target).\n')
        fh.write('static const unsigned ss_site_vrams[SS_SITE_COUNT + 1][3] = {{0, 0, 0},\n')
        for f, c in sites:
            cv = funcs[c.callee].vram if (c.kind == 'direct' and c.callee in funcs) else 0
            fh.write('    {0x%08Xu, 0x%08Xu, 0x%08Xu}, // %d %s -> %s\n' % (f.vram or 0, c.vram or 0, cv or 0, c.site, f.name, c.callee))
        fh.write('};\n')
        # Code segments sharing a VRAM (the three overlays at 0x802EFE70, the stadium blobs at
        # 0x80377D20): which one each site's function belongs to, so a chain through code that has
        # since been replaced (a process the game will kill without waking) can be refused.
        fh.write('// site -> segment of its function: 0 static, 1-3 ovl_a/b/c, 11-15 stadium1-5.\n')
        fh.write('static const unsigned char ss_site_segment[SS_SITE_COUNT + 1] = {0,\n')
        def segment(name):
            m = re.match(r'ovl_([abc])_', name)
            if m:
                return ' abc'.index(m.group(1))
            m = re.match(r'ovl_stadium(\d)_', name)
            return 10 + int(m.group(1)) if m else 0
        for i in range(0, len(sites), 32):
            fh.write('    ' + ', '.join(str(segment(f.name)) for f, c in sites[i:i + 32]) + ',\n')
        fh.write('};\n')
    # site -> the function it is in, as a pointer: a resume calls these rather than looking an
    # address up, since overlays share addresses (SS_INDIRECT in ss_runtime.h).
    with open(os.path.join(out_dir, 'ss_site_funcs.c'), 'w') as fh:
        fh.write('// Generated by tools/savestate/resumable.py\n#include "funcs.h"\n#include "ss_sites.h"\n\n')
        fh.write('void* const ss_site_func[SS_SITE_COUNT + 1] = {0,\n')
        for f, c in sites:
            fh.write('    (void*)%s,\n' % f.name)
        fh.write('};\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='src', default=os.path.join(ROOT, 'build/RecompiledFuncs'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/savestate/RecompiledFuncs'))
    ap.add_argument('--rom', default=os.path.join(ROOT, 'build/baserom.z64'))
    ap.add_argument('--sinks', default='sleep', help="'sleep' (process_sleep only) or 'sleep,recv' (also osRecvMesg_recomp)")
    ap.add_argument('--indirect', default='conservative', choices=['conservative', 'direct'],
                    help='conservative: every indirect call may sleep; direct: ignore indirect calls')
    ap.add_argument('--report', default=None, help='write a JSON report here')
    ap.add_argument('--no-emit', action='store_true')
    a = ap.parse_args()

    files, funcs = parse(a.src)
    sink_pairs = dict(SINKS_SLEEP)
    sink_callees = set()
    if 'recv' in a.sinks.split(','):
        sink_callees.add(SINK_RECV_CALLEE)

    S_direct = compute_set(funcs, sink_pairs, set(), False)
    S_direct_recv = compute_set(funcs, sink_pairs, {SINK_RECV_CALLEE}, False)
    S = compute_set(funcs, sink_pairs, sink_callees, a.indirect == 'conservative')
    sites = assign_sites(funcs, S, sink_pairs, sink_callees, sink_pairs)

    rep = collections.OrderedDict()
    rep['functions_total'] = len(funcs)
    rep['calls_total'] = sum(len(f.calls) for f in funcs.values())
    rep['S_direct_only'] = len(S_direct)
    rep['S_direct_incl_osRecvMesg'] = len(S_direct_recv)
    rep['S_used'] = len(S)
    rep['S_used_mode'] = dict(sinks=a.sinks, indirect=a.indirect)
    kinds = collections.Counter((c.kind, c.mode, c.follow) for _, c in sites)
    rep['sites_total'] = len(sites)
    rep['sites_by_kind'] = {'%s/%s/%s' % k: v for k, v in sorted(kinds.items())}
    rep['calls_in_S_not_instrumented'] = sum(1 for n in S for c in funcs[n].calls if c.site is None)

    # indirect calls
    ind_sites = [(f, c) for f, c in sites if c.kind == 'indirect']
    rep['indirect_sites_in_S'] = [dict(site=c.site, func=f.name, file='%s:%d' % (f.file, c.line + 1),
                                       vram='0x%08X' % (c.vram or 0), insn=c.insn, expr=c.callee,
                                       tail=c.follow == 'return', func_reaches_sleep_directly=f.name in S_direct)
                                  for f, c in ind_sites]
    code_refs, data_refs = address_taken(files, funcs, a.src, a.rom)
    taken = set(code_refs) | set(data_refs)
    rep['address_taken_functions'] = len(taken)
    rep['address_taken_and_can_sleep'] = sorted(
        [dict(func=n, vram='0x%08X' % (funcs[n].vram or 0), code_refs=sorted(code_refs.get(n, ()))[:6],
              data_refs=len(data_refs.get(n, ()))) for n in taken if n in S_direct],
        key=lambda d: d['vram'])

    # constructs to check
    problems = []
    for n in sorted(S):
        f = funcs[n]
        if f.has_vla:
            problems.append(dict(kind='vla', where='%s:%d' % (f.file, f.start + 1), func=n))
        for c in f.calls:
            if c.site is not None and c.indent != '    ':
                problems.append(dict(kind='call_in_nested_block', where='%s:%d' % (f.file, c.line + 1), func=n))
            if c.site is not None and c.vram is None:
                problems.append(dict(kind='no_call_insn_comment', where='%s:%d' % (f.file, c.line + 1), func=n))
        extra = f.locals - {'hi', 'lo', 'result', 'c1cs'} - {x for x in f.locals if x.startswith('jr_addend_')}
        if extra:
            problems.append(dict(kind='unknown_locals', where='%s:%d' % (f.file, f.start + 1), func=n, locals=sorted(extra)))
        for ln, h in f.hooks:
            problems.append(dict(kind='host_hook_in_S', where='%s:%d' % (f.file, ln + 1), func=n, hook=h))
    # liveness of C locals across instrumented calls
    by_func = collections.defaultdict(list)
    for f, c in sites:
        by_func[f.name].append(c)
    live = []
    for n, cs in by_func.items():
        f = funcs[n]
        for c, vs in liveness_issues(files[f.file], f, cs, f.locals):
            live.append(dict(where='%s:%d' % (f.file, c.line + 1), func=n, site=c.site,
                             call_vram='0x%08X' % (c.vram or 0), callee=c.callee, live=vs))
    rep['locals_live_across_instrumented_calls'] = live
    rep['problems'] = problems

    # host code (src/) that calls recompiled functions in S: a host frame between game frames
    host = []
    srcdir = os.path.join(ROOT, 'src')
    for dp, _, fns in os.walk(srcdir):
        for fn in fns:
            if not fn.endswith(('.cpp', '.c')):
                continue
            p = os.path.join(dp, fn)
            for k, l in enumerate(open(p, errors='replace')):
                for m in re.finditer(r'\b(\w+)\(rdram, ctx\)', l):
                    if m.group(1) in S_direct:
                        host.append(dict(where='%s:%d' % (os.path.relpath(p, ROOT), k + 1), calls=m.group(1)))
    rep['host_code_calling_sleeping_functions'] = host

    # roots: S functions nobody calls directly (process entries, indirect-only targets)
    called = {c.callee for f in funcs.values() for c in f.calls if c.kind == 'direct'}
    rep['S_direct_roots'] = sorted('%s@0x%08X' % (n, funcs[n].vram or 0) for n in S_direct if n not in called)

    if not a.no_emit:
        emit(files, funcs, sites, a.src, a.out)
        rep['output'] = a.out
    if a.report:
        os.makedirs(os.path.dirname(os.path.abspath(a.report)), exist_ok=True)
        with open(a.report, 'w') as fh:
            json.dump(rep, fh, indent=1)
    summ = {k: v for k, v in rep.items() if not isinstance(v, list)}
    summ['indirect_sites_in_S'] = len(rep['indirect_sites_in_S'])
    summ['address_taken_and_can_sleep'] = len(rep['address_taken_and_can_sleep'])
    summ['locals_live_across_instrumented_calls'] = len(live)
    summ['problems'] = dict(collections.Counter(p['kind'] for p in problems))
    summ['host_code_calling_sleeping_functions'] = len(host)
    summ['S_direct_roots'] = len(rep['S_direct_roots'])
    print(json.dumps(summ, indent=1))


if __name__ == '__main__':
    main()
