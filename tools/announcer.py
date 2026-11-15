#!/usr/bin/env python3
"""Announcer banks for the 2026 app: your own recordings in, a game speech bank out (docs/ANNOUNCER.md).

  announcer.py build NAME OUTDIR --city C.wav --nickname N.wav --full F.wav [--full2 F2.wav]
                    [--player 11=Q.wav ...] [--rom ROM [--like cardinal]] [--no-match]
      Process each clip so it sits with the game's announcer, then encode the bank: NAME.wbk/.ptr/.bfx.
      NAME is the team's bank name, the nickname's first 8 letters, lowercased ("texans", "commande").
      Processing: trim silence, band-limit to the game's 8751 Hz voice, compress, match the tone (6-band
      EQ) and loudness of the game's own calls (--rom: your ROM; --like: which team's calls, default
      cardinal), resample. Copy the three files into <app folder>/announcer/ (launcher → Custom Content opens the app folder).
  announcer.py extract ROM NAME OUTDIR   decode a bank's calls to WAVs (fx0 city, fx1 nickname, fx2/fx3 full name)
  announcer.py preview ROM NAME BANKDIR [--like cardinal]
      Play the game's full-name call, then yours, so you can compare by ear (macOS afplay).

Input WAVs can be any rate, mono or stereo; macOS afconvert does the resampling. The clips you need:
city ("Houston"), nickname ("Texans"), and the full name ("Houston Texans"), ideally twice: a neutral read
(--full, said after "versus" isn't needed) and a hyped one (--full2). Our own voices only: don't feed this
the game's announcer, or anyone else's voice without their permission.
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "announcer"))
import bank  # noqa: E402
import build_bank  # noqa: E402
import fs  # noqa: E402

RATE = 8751  # the team banks' playback rate (basenote 0xF0)
BANDS = [180, 350, 700, 1400, 2400, 3600]


# ---- I/O ----

def read_any(path):
    """Any WAV/AIFF/M4A -> mono 16-bit PCM at RATE, via afconvert."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "x.wav")
        subprocess.run(["afconvert", "-f", "WAVE", "-d", f"LEI16@{RATE}", "-c", "1", path, out], check=True)
        with wave.open(out) as w:
            return [s / 32768.0 for s in struct.unpack("<%dh" % w.getnframes(), w.readframes(w.getnframes()))]


def write_wav(path, x, rate=RATE):
    bank.write_wav(path, [max(-32768, min(32767, int(round(v * 32767)))) for v in x], rate)


# ---- DSP (plain Python; clips are about a second long) ----

def biquad(x, kind, f, q=0.707, gain_db=0.0, rate=RATE):
    a = 10 ** (gain_db / 40)
    w0 = 2 * math.pi * f / rate
    alpha = math.sin(w0) / (2 * q)
    c = math.cos(w0)
    if kind == "hp":
        b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]; ad = [1 + alpha, -2 * c, 1 - alpha]
    elif kind == "lp":
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2]; ad = [1 + alpha, -2 * c, 1 - alpha]
    elif kind == "bp":
        b = [alpha, 0, -alpha]; ad = [1 + alpha, -2 * c, 1 - alpha]
    else:  # peaking
        b = [1 + alpha * a, -2 * c, 1 - alpha * a]; ad = [1 + alpha / a, -2 * c, 1 - alpha / a]
    b = [v / ad[0] for v in b]; a1, a2 = ad[1] / ad[0], ad[2] / ad[0]
    y, x1, x2, y1, y2 = [], 0.0, 0.0, 0.0, 0.0
    for v in x:
        o = b[0] * v + b[1] * x1 + b[2] * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y.append(o)
    return y


def trim(x, threshold_db=-38, pad_s=0.03):
    """Cut leading and trailing silence. The threshold follows the take: 12 dB above its noise floor
    (the quietest tenth of 10 ms windows) or 24 dB under its peak, whichever is higher, and never
    below threshold_db, so a laptop mic's room noise is cut too."""
    win = int(0.01 * RATE)
    peaks = [max(abs(v) for v in x[i:i + win]) for i in range(0, max(1, len(x) - win), win)]
    if not peaks:
        return x
    ranked = sorted(peaks)
    noise, peak = ranked[len(ranked) // 10], ranked[-1]
    t = max(10 ** (threshold_db / 20), noise * 10 ** (12 / 20), peak * 10 ** (-24 / 20))
    loud = [k * win for k, p in enumerate(peaks) if p > t]
    if not loud:
        return x
    pad = int(pad_s * RATE)
    return x[max(0, loud[0] - pad):min(len(x), loud[-1] + win + pad)]


def compress(x, threshold_db=-20, ratio=4.0, attack_s=0.003, release_s=0.08):
    t = 10 ** (threshold_db / 20)
    ga, gr = math.exp(-1 / (attack_s * RATE)), math.exp(-1 / (release_s * RATE))
    env, y = 0.0, []
    for v in x:
        a = abs(v)
        env = ga * env + (1 - ga) * a if a > env else gr * env + (1 - gr) * a
        g = 1.0 if env <= t else (t * (env / t) ** (1 / ratio)) / env
        y.append(v * g)
    return y


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


def band_profile(clips):
    """Energy (dB) per band over all clips, relative to the total."""
    allx = [v for c in clips for v in c]
    total = rms(allx) or 1e-9
    return [20 * math.log10((rms(biquad(allx, "bp", f, q=1.2)) or 1e-9) / total) for f in BANDS]


def tone_match(x, ref_profile, max_db=9.0):
    mine = band_profile([x])
    for f, want, have in zip(BANDS, ref_profile, mine):
        g = max(-max_db, min(max_db, want - have))
        if abs(g) > 0.5:
            x = biquad(x, "peak", f, q=1.0, gain_db=g)
    return x


def normalise(x, target_rms, peak_limit=0.95):
    g = target_rms / (rms(x) or 1e-9)
    peak = max(abs(v) for v in x) * g
    if peak > peak_limit:
        g *= peak_limit / peak
    return [v * g for v in x]


def reference(rom_path, like):
    rom = fs.load(rom_path)
    _, ents = fs.toc(rom)
    b = bank.load_bank(rom, ents, like)
    if b["ptr"] is None:
        raise SystemExit(f"no bank '{like}' in {rom_path}")
    p, fx = bank.parse_ptr(b["ptr"]), bank.parse_bfx(b["bfx"])
    clips = []
    for e in fx["effects"][:4]:
        wv, _, _ = bank.fx_wave_note(e)
        w = p["waves"][fx["wave_table"][wv]] if wv is not None else None
        if w:
            pcm = bank.decode_vadpcm(b["wbk"][w["base"]:w["base"] + w["len"]], w["book"], 2, 4)
            clips.append([s / 32768.0 for s in pcm])
    return clips


def process(path, ref_profile, ref_rms):
    x = read_any(path)
    x = trim(x)
    x = biquad(x, "hp", 120)
    x = biquad(biquad(x, "lp", 3700), "lp", 3700)
    x = compress(x)
    if ref_profile:
        x = tone_match(x, ref_profile)
    return normalise(x, ref_rms)


# ---- commands ----

def cmd_build(a):
    clips = {0: a.city, 1: a.nickname, 2: a.full, 3: a.full2 or a.full}
    for p in a.player or []:
        k, path = p.split("=", 1)
        if not 11 <= int(k) <= 14:
            raise SystemExit("--player takes 11-14 (the calls for player ids 4-7)")
        clips[int(k)] = path
    ref_profile, ref_rms = None, 0.18
    if a.rom and not a.no_match:
        ref = reference(a.rom, a.like)
        ref_profile = band_profile(ref)
        ref_rms = sum(rms(c) for c in ref) / len(ref)
        print(f"matching the tone and loudness of '{a.like}' (RMS {20 * math.log10(ref_rms):.1f} dBFS)")
    pcm = {}
    os.makedirs(a.outdir, exist_ok=True)
    for fx_id, path in clips.items():
        x = process(path, ref_profile, ref_rms)
        write_wav(os.path.join(a.outdir, f"{a.name}_fx{fx_id:02d}.wav"), x)
        pcm[fx_id] = [max(-32768, min(32767, int(round(v * 32767)))) for v in x]
    ptr, wbk, bfx, report = build_bank.build_from_pcm(pcm, RATE)
    for ext, data in (("ptr", ptr), ("wbk", wbk), ("bfx", bfx)):
        open(os.path.join(a.outdir, f"{a.name}.{ext}"), "wb").write(data)
    for k, (dur, snr) in sorted(report.items()):
        print(f"fx{k:02d} {dur:.2f} s  (codec SNR {snr:.0f} dB)")
    full = report[2][0]
    print(f"wrote {a.name}.wbk/.ptr/.bfx to {a.outdir}. Copy them into <app folder>/announcer/.")
    print(f"VS screen: the team's pause after its name should be about {round(54 * full)} frames (team block +0x1D6).")


def cmd_extract(a):
    rom = fs.load(a.rom)
    _, ents = fs.toc(rom)
    b = bank.load_bank(rom, ents, a.name)
    if b["ptr"] is None:
        raise SystemExit(f"no bank '{a.name}'")
    p, fx = bank.parse_ptr(b["ptr"]), bank.parse_bfx(b["bfx"])
    os.makedirs(a.outdir, exist_ok=True)
    for i, e in enumerate(fx["effects"]):
        wv, note, _ = bank.fx_wave_note(e)
        if wv is None:
            continue
        w = p["waves"][fx["wave_table"][wv]]
        pcm = bank.decode_vadpcm(b["wbk"][w["base"]:w["base"] + w["len"]], w["book"], 2, 4)
        rate = bank.OUT_RATE * 2 ** ((w["pitch_offset"] + note) / 12)  # the effect's note-on adds its note
        bank.write_wav(os.path.join(a.outdir, f"fx{i:02d}.wav"), pcm, rate)
    print(f"wrote {a.outdir}/fxNN.wav (fx0 city, fx1 nickname, fx2/fx3 full name, fx11-14 player calls)")


def cmd_preview(a):
    ref = reference(a.rom, a.like)
    with tempfile.TemporaryDirectory() as tmp:
        mine = os.path.join(a.bankdir, f"{a.name}_fx02.wav")
        theirs = os.path.join(tmp, "game.wav")
        write_wav(theirs, ref[2])
        for path in (theirs, mine):
            print("playing", "the game's call" if path == theirs else "yours")
            subprocess.run(["afplay", path], check=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("name")
    b.add_argument("outdir")
    b.add_argument("--city", required=True)
    b.add_argument("--nickname", required=True)
    b.add_argument("--full", required=True)
    b.add_argument("--full2")
    b.add_argument("--player", action="append", metavar="N=WAV")
    b.add_argument("--rom")
    b.add_argument("--like", default="cardinal")
    b.add_argument("--no-match", action="store_true")
    e = sub.add_parser("extract")
    e.add_argument("rom")
    e.add_argument("name")
    e.add_argument("outdir")
    p = sub.add_parser("preview")
    p.add_argument("rom")
    p.add_argument("name")
    p.add_argument("bankdir")
    p.add_argument("--like", default="cardinal")
    a = ap.parse_args(argv)
    if len(a.name) > 8 or a.name != a.name.lower():
        raise SystemExit("NAME is the bank name: the nickname's first 8 letters, lowercase (texans, commande)")
    {"build": cmd_build, "extract": cmd_extract, "preview": cmd_preview}[a.cmd](a)


if __name__ == "__main__":
    main()
