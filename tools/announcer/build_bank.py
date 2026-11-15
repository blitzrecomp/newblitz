"""Prototype: build a Blitz 2000 (libmus) team speech bank (.ptr/.wbk/.bfx) from WAV files.

  python3 build_bank.py OUTDIR NAME --rate 22050 fx0=city.wav fx1=nick.wav fx2=full_a.wav fx3=full_b.wav [fx11=qb.wav ...]

Each WAV must be mono 16-bit PCM at --rate. --rate must be 22050*2^(n/12) for an integer n in -48..0 (22050, 11025,
8751 = 22050*2^(-16/12) like the vanilla team banks, ...); n is stored as the wave's basenote (n & 0xFF).
Layout follows the vanilla team banks: 21 effects (0..20); effects without a WAV are empty (a single 0x80 'stop'),
which plays nothing, exactly like vanilla effects 4..9. Analysis prototype only (not used by the game build).
Also: --selftest (round-trip checks against the vanilla ROM in build/)."""
import math, os, struct, sys, wave
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bank

NEFF = 21
TICKS_PER_S = 96.0  # libmus fx note length unit; vanilla team banks: median 10.4 ms per tick

# ---------------- codebook design (order 2, 4 predictors) ----------------
def lpc2(x):
    r0 = sum(v * v for v in x); r1 = sum(x[i] * x[i - 1] for i in range(1, len(x))); r2 = sum(x[i] * x[i - 2] for i in range(2, len(x)))
    if r0 <= 0: return None
    k1 = r1 / r0
    e1 = r0 * (1 - k1 * k1)
    if e1 <= 0: return None
    k2 = (r2 - k1 * r1) / e1
    k1 = max(-0.995, min(0.995, k1)); k2 = max(-0.995, min(0.995, k2))
    a1 = k1 * (1 - k2); a2 = k2  # x[n] ~ a1 x[n-1] + a2 x[n-2]
    return a1, a2, r0

def design_book(pcm, npred=4, frame=16, win=64, iters=20):
    feats = []
    for st in range(0, len(pcm) - win, frame):
        r = lpc2(pcm[st:st + win])
        if r and r[2] > win * 200 ** 2: feats.append(r)
    if len(feats) < npred: feats += [(0.0, 0.0, 1.0)] * npred
    feats.sort(key=lambda f: f[0])  # init centroids spread over a1
    cents = [feats[int((i + 0.5) * len(feats) / npred)][:2] for i in range(npred)]
    for _ in range(iters):
        acc = [[0.0, 0.0, 0.0] for _ in cents]
        for a1, a2, e in feats:
            j = min(range(npred), key=lambda j: (a1 - cents[j][0]) ** 2 + (a2 - cents[j][1]) ** 2)
            w = math.log(e); acc[j][0] += a1 * w; acc[j][1] += a2 * w; acc[j][2] += w
        cents = [(c[0] / c[2], c[1] / c[2]) if c[2] else cents[j] for j, c in enumerate(acc)]
    book = []
    for a1, a2 in cents:
        h2 = [0.0] * 8; h1 = [0.0] * 8  # impulse responses to x[-2]=1 and x[-1]=1
        for src, h in ((2, h2), (1, h1)):
            p2, p1 = (1.0, 0.0) if src == 2 else (0.0, 1.0)
            for k in range(8):
                v = a1 * p1 + a2 * p2; h[k] = v; p2, p1 = p1, v
        book += [max(-32768, min(32767, int(round(v * 2048)))) for v in h2]
        book += [max(-32768, min(32767, int(round(v * 2048)))) for v in h1]
    return book

# ---------------- VADPCM encoder (greedy, matches bank.decode_vadpcm) ----------------
def encode_vadpcm(pcm, book, order=2, npred=4):
    tables = bank.expand_book(book, order, npred)
    n = (len(pcm) + 15) // 16 * 16
    pcm = list(pcm) + [0] * (n - len(pcm))
    state = [0] * 16; out = bytearray()
    for f in range(n // 16):
        x = pcm[f * 16:(f + 1) * 16]
        best = None
        for p in range(npred):
            # estimate scale from the unquantised residual
            emax = 0; prev = state[-2:]
            seq = prev + x
            for i in range(2, 18):
                a1 = book[p * 16 + 8] / 2048; a2 = book[p * 16] / 2048
                emax = max(emax, abs(seq[i] - a1 * seq[i - 1] - a2 * seq[i - 2]))
            s0 = 0
            while s0 < 12 and emax / (1 << s0) > 7.5: s0 += 1
            for s in range(max(0, s0 - 1), min(12, s0 + 1) + 1):
                scale = 1 << s; ix = [0] * 16; cur = [0] * 16; err = 0
                for j in range(2):
                    in_vec = [0] * (order + 8)
                    for i in range(order):
                        in_vec[i] = state[16 - order + i] if j == 0 else cur[8 - order + i]
                    for i in range(8):
                        t = tables[p][i]
                        base = sum(t[k] * in_vec[k] for k in range(order + i))  # contributions known so far
                        q = int(round((x[j * 8 + i] * 2048 - base) / (2048.0 * scale)))
                        q = max(-8, min(7, q))
                        ix[j * 8 + i] = q; in_vec[order + i] = q * scale
                        cur[j * 8 + i] = bank.inner(t, in_vec)
                        err += (cur[j * 8 + i] - x[j * 8 + i]) ** 2
                if best is None or err < best[0]:
                    best = (err, p, s, ix, cur)
        err, p, s, ix, cur = best
        state = cur
        fr = bytearray([(s << 4) | p])
        for i in range(0, 16, 2):
            fr.append(((ix[i] & 0xF) << 4) | (ix[i + 1] & 0xF))
        out += fr
    return bytes(out)

# ---------------- file writers ----------------
def fx_bytes(wave_index, length_ticks, volume=0x7F):
    # Cwave w, Cdefa envelope (copied from vanilla), Cpan 0x7F, Cvolume v, note 0x30 (plays at the wave's native rate), length, Cstop
    d = bytearray([0x81, wave_index, 0x84, 0x01, 0x7F, 0x01, 0x7F, 0x01, 0x7F, 0x10, 0x9C, 0x7F, 0xA6, volume, 0x30])
    if length_ticks >= 0x80:
        assert length_ticks < 0x8000
        d += bytes([0x80 | (length_ticks >> 8), length_ticks & 0xFF])
    else:
        d.append(length_ticks)
    d.append(0x80)
    return bytes(d)

def write_bank(waves, effects, nwaves_table=None):
    """waves: list of dict(adpcm=bytes, book=[64 s16], basenote=int, detune=int).
    effects: list of NEFF items, each None or (wave_index, length_ticks). Returns (ptr, wbk, bfx) bytes."""
    n = len(waves)
    # .wbk: 16-byte header, each wave 16-byte aligned
    wbk = bytearray(b'N64 WaveTables \0'); bases = []
    for i, w in enumerate(waves):
        bases.append(len(wbk)); wbk += w['adpcm']
        while len(wbk) % (16 if i + 1 < len(waves) else 2): wbk.append(0)  # vanilla: 16-byte aligned waves, file padded to even size
    # .ptr
    ptr = bytearray(b'N64 PtrTablesV2\0') + bytes(16)
    ptr += struct.pack('>i', n) + bytes(12)  # count, then 3 offsets patched below
    woffs = []
    for i, w in enumerate(waves):
        wo = len(ptr); woffs.append(wo)
        ptr += struct.pack('>IIBBHII', bases[i], len(w['adpcm']), 0, 0, 0, 0, wo + 0x18)
        ptr += bytes(0x18 - 0x14)
        ptr += struct.pack('>ii', 2, 4) + struct.pack('>64h', *w['book'])
    wl = len(ptr); ptr += b''.join(struct.pack('>I', o) for o in woffs)
    bn = len(ptr); ptr += bytes(w['basenote'] & 0xFF for w in waves)
    while len(ptr) % 4: ptr.append(0)
    dt = len(ptr); ptr += b''.join(struct.pack('>bxxx', w.get('detune', 0)) for w in waves)
    struct.pack_into('>III', ptr, 0x24, bn, dt, wl)
    # .bfx
    hdr = 0x18 + 8 * len(effects)
    data = bytearray(); offs = []
    for e in effects:
        offs.append(hdr + len(data))
        data += fx_bytes(*e) if e else b'\x80'
    wt = hdr + len(data)
    if wt % 2: data.append(len(effects) & 0xFF); wt += 1  # vanilla pad byte observed = 0x15 (the effect count)
    nwt = nwaves_table if nwaves_table is not None else n
    bfx = bytearray(struct.pack('>IIIIII', len(effects), len(effects), nwt, 0, 0, wt))
    for o in offs: bfx += struct.pack('>II', o, 100)
    bfx += data + b''.join(struct.pack('>H', i) for i in range(nwt))
    return bytes(ptr), bytes(wbk), bytes(bfx)

def read_wav(path):
    with wave.open(path) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2, path + ': need mono 16-bit'
        return w.getframerate(), list(struct.unpack('<%dh' % w.getnframes(), w.readframes(w.getnframes())))

def snr(a, b):
    n = min(len(a), len(b)); sig = sum(v * v for v in a[:n]); err = sum((a[i] - b[i]) ** 2 for i in range(n))
    return 10 * math.log10(sig / err) if err else float('inf')

def build_from_pcm(clips, rate):
    """clips: {effect_id: pcm list}. Returns ptr, wbk, bfx and per-clip SNR."""
    n = round(12 * math.log2(rate / bank.OUT_RATE))
    assert abs(bank.OUT_RATE * 2 ** (n / 12) - rate) < 2 and -48 <= n <= 0, 'rate must be 22050*2^(n/12)'
    waves, effects, report = [], [None] * NEFF, {}
    for fx_id in sorted(clips):
        pcm = clips[fx_id]
        book = design_book(pcm)
        adpcm = encode_vadpcm(pcm, book)
        dec = bank.decode_vadpcm(adpcm, book, 2, 4)
        report[fx_id] = (len(pcm) / rate, snr(pcm, dec))
        effects[fx_id] = (len(waves), int(math.ceil(len(pcm) / rate * TICKS_PER_S)) + 4)
        waves.append(dict(adpcm=adpcm, book=book, basenote=n & 0xFF, detune=0))
    ptr, wbk, bfx = write_bank(waves, effects)
    return ptr, wbk, bfx, report

def selftest():
    import fs
    here = os.path.dirname(os.path.abspath(__file__))
    rom = fs.load(os.path.join(here, '../../baserom.z64')); _, ents = fs.toc(rom)
    names = sorted(n[:-4] for n in fs.by_name(ents) if n.endswith('.bfx') and fs.by_name(ents)[n]['usize'] in (484, 486))
    for name in names:
        b = bank.load_bank(rom, ents, name); p = bank.parse_ptr(b['ptr']); fx = bank.parse_bfx(b['bfx'])
        waves = [dict(adpcm=b['wbk'][w['base']:w['base'] + w['len']], book=w['book'], basenote=w['basenote'], detune=w['detune']) for w in p['waves']]
        effects = []
        for e in fx['effects']:
            wv, note, ln = bank.fx_wave_note(e)
            vol = [c[1] for c in e['cmds'] if c[0] == 'cmda6']
            effects.append(None if wv is None else (wv, ln, vol[0]))
        ptr, wbk, bfx = write_bank(waves, effects)
        print('%-9s writer reproduces vanilla bytes: ptr %s  wbk %s  bfx %s' % (name, ptr == b['ptr'], wbk == b['wbk'], bfx == b['bfx']))
    # re-encode cardinal's decoded waves: own book vs designed book
    b = bank.load_bank(rom, ents, 'cardinal'); p = bank.parse_ptr(b['ptr'])
    for w in p['waves'][:4]:
        pcm = bank.decode_vadpcm(b['wbk'][w['base']:w['base'] + w['len']], w['book'], 2, 4)
        a = encode_vadpcm(pcm, w['book']); d1 = bank.decode_vadpcm(a, w['book'], 2, 4)
        bk = design_book(pcm); a2 = encode_vadpcm(pcm, bk); d2 = bank.decode_vadpcm(a2, bk, 2, 4)
        print('cardinal wave %2d: re-encode SNR with original book %.1f dB, with designed book %.1f dB' % (w['index'], snr(pcm, d1), snr(pcm, d2)))

if __name__ == '__main__':
    if sys.argv[1:2] == ['--selftest']:
        selftest(); sys.exit(0)
    outdir, name = sys.argv[1], sys.argv[2]
    rate = 22050; clips = {}
    args = sys.argv[3:]
    while args:
        a = args.pop(0)
        if a == '--rate': rate = int(args.pop(0)); continue
        k, path = a.split('=', 1)
        r, pcm = read_wav(path)
        assert r == rate, '%s is %d Hz, expected %d' % (path, r, rate)
        clips[int(k[2:])] = pcm
    ptr, wbk, bfx, report = build_from_pcm(clips, rate)
    os.makedirs(outdir, exist_ok=True)
    for ext, data in (('ptr', ptr), ('wbk', wbk), ('bfx', bfx)):
        open(os.path.join(outdir, '%s.%s' % (name, ext)), 'wb').write(data)
    for k, (dur, s) in sorted(report.items()):
        print('fx%02d %.3f s  re-decode SNR %.1f dB' % (k, dur, s))
    print('wrote %s.{ptr,wbk,bfx}: %d / %d / %d bytes' % (name, len(ptr), len(wbk), len(bfx)))
