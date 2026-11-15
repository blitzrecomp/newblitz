"""Parse libmus banks used by NFL Blitz 2000 (N64): .ptr (N64 PtrTablesV2), .wbk (N64 WaveTables), .bfx (fx bank).
Decode VADPCM waves to WAV. Analysis only: outputs go under build/analysis/announcer/."""
import struct, wave, os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fs

OUT_RATE = 22050  # libmus syn_output_rate (0x5622) set at 0x80004AD4

def s8(x): return x - 256 if x & 0x80 else x

def parse_ptr(b):
    assert b[:15] == b'N64 PtrTablesV2', b[:16]
    flags, = struct.unpack_from('>I', b, 0x10)
    count, bn_off, dt_off, wl_off = struct.unpack_from('>iIII', b, 0x20)
    waves = []
    for i in range(count):
        wo, = struct.unpack_from('>I', b, wl_off + 4 * i)
        base, length = struct.unpack_from('>II', b, wo)
        typ, wflags = b[wo + 8], b[wo + 9]
        loop_off, book_off = struct.unpack_from('>II', b, wo + 12)
        w = dict(index=i, wave_off=wo, base=base, len=length, type=typ, flags=wflags,
                 loop_off=loop_off, book_off=book_off,
                 basenote=b[bn_off + i], detune=s8(b[dt_off + 4 * i]))
        if typ == 0 and book_off:
            order, npred = struct.unpack_from('>ii', b, book_off)
            n = order * npred * 8
            w['order'], w['npred'] = order, npred
            w['book'] = list(struct.unpack_from('>%dh' % n, b, book_off + 8))
        if loop_off:
            start, end, cnt = struct.unpack_from('>IIi', b, loop_off)
            w['loop'] = (start, end, cnt)
        # libmus 0x80008E0C: per-wave pitch offset = detune*0.01 + s8(basenote - 0x30); note-on adds the note.
        semis = w['detune'] * 0.01 + s8((w['basenote'] - 0x30) & 0xFF)
        w['pitch_offset'] = semis
        waves.append(w)
    return dict(flags=flags, count=count, basenote_off=bn_off, detune_off=dt_off, wavelist_off=wl_off, waves=waves)

def parse_bfx(b):
    ncomp, neff, nwaves, flags, ptrbank, wt_off = struct.unpack_from('>IIIIII', b, 0)
    effects = []
    offs = [struct.unpack_from('>II', b, 0x18 + 8 * i) for i in range(ncomp)]
    wave_table = list(struct.unpack_from('>%dH' % nwaves, b, wt_off))
    ends = sorted(set([o for o, _ in offs] + [wt_off]))
    for i, (o, pri) in enumerate(offs):
        nxt = min([e for e in ends if e > o] + [len(b)])
        data = b[o:nxt]
        effects.append(dict(index=i, off=o, priority=pri, data=data, cmds=decode_fx(data, wave_table)))
    return dict(ncomp=ncomp, neff=neff, nwaves=nwaves, wave_table=wave_table, effects=effects)

# libmus command lengths (argument bytes) for the commands seen in these banks; others are reported raw.
CMD_ARGS = {0x80: 0, 0x81: 1, 0x84: 7, 0x9c: 1, 0xa6: 1, 0x8b: 1, 0x8d: 1, 0xa2: 1, 0x85: 1, 0x86: 1}
def decode_fx(d, wave_table):
    cmds, i = [], 0
    while i < len(d):
        c = d[i]
        if c < 0x80:  # note + length (1 byte, or 2 bytes when bit 7 set)
            if i + 1 >= len(d): cmds.append(('note?', c)); break
            l = d[i + 1]
            if l & 0x80:
                l = ((l & 0x7F) << 8) | d[i + 2]; i += 3
            else:
                i += 2
            cmds.append(('note', c, l))
        elif c == 0x80:
            cmds.append(('stop',)); i += 1
            break
        elif c in CMD_ARGS:
            n = CMD_ARGS[c]
            args = list(d[i + 1:i + 1 + n])
            if c == 0x81:
                cmds.append(('wave', args[0], wave_table[args[0]] if args[0] < len(wave_table) else None))
            else:
                cmds.append(('cmd%02x' % c, *args))
            i += 1 + n
        else:
            cmds.append(('raw', d[i:].hex())); break
    return cmds

def fx_wave_note(e):
    w = n = l = None
    for c in e['cmds']:
        if c[0] == 'wave': w = c[2]
        if c[0] == 'note' and n is None: n, l = c[1], c[2]
    return w, n, l

# ---- VADPCM (N64 SDK vadpcm_dec semantics) ----
def expand_book(book, order, npred):
    tables = []
    for p in range(npred):
        t = [[0] * (order + 8) for _ in range(8)]
        for j in range(order):
            for k in range(8):
                t[k][j] = book[p * order * 8 + j * 8 + k]
        for k in range(1, 8):
            t[k][order] = t[k - 1][order - 1]
        t[0][order] = 2048
        for k in range(1, 8):
            for j in range(k):
                t[j][k + order] = 0
            for j in range(k, 8):
                t[j][k + order] = t[j - k][order]
        tables.append(t)
    return tables

def inner(v1, v2):
    out = sum(a * b for a, b in zip(v1, v2))
    d = out >> 11  # floor division by 2048 (matches vadpcm_dec's correction for negatives)
    return d

def decode_vadpcm(data, book, order, npred):
    tables = expand_book(book, order, npred)
    out = []
    state = [0] * 16
    for f in range(len(data) // 9):
        fr = data[f * 9:(f + 1) * 9]
        scale = 1 << (fr[0] >> 4)
        pred = fr[0] & 0xF
        ix = []
        for c in fr[1:]:
            for nib in (c >> 4, c & 0xF):
                if nib >= 8: nib -= 16
                ix.append(nib * scale)
        cur = [0] * 16
        for j in range(2):
            in_vec = [0] * (order + 8)
            for i in range(order):
                in_vec[i] = state[16 - order + i] if j == 0 else cur[8 - order + i]
            for i in range(8):
                in_vec[order + i] = ix[j * 8 + i]
            for i in range(8):
                cur[j * 8 + i] = inner(tables[pred][i], in_vec)
        state = cur
        out.extend(max(-32768, min(32767, s)) for s in cur)
    return out

def write_wav(path, samples, rate):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(round(rate)))
        w.writeframes(struct.pack('<%dh' % len(samples), *samples))

def load_bank(rom, ents, name):
    d = fs.by_name(ents)
    out = {}
    for ext in ('ptr', 'wbk', 'bfx'):
        e = d.get(f'{name}.{ext}')
        out[ext] = fs.read(rom, e) if e else None
        out[ext + '_ent'] = e
    return out
