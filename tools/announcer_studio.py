#!/usr/bin/env python3
"""Announcer Studio: record your own team calls in the browser and build them into a speech bank.

  .venv/bin/python tools/announcer_studio.py [--rom ROM] [--app-folder DIR] [--port 8765]

Opens http://127.0.0.1:8765 (this computer only). The page explains what a team bank holds, plays
the game's own calls from your ROM as examples, records takes from your microphone, and builds and
installs the bank with tools/announcer.py (docs/ANNOUNCER.md).

Files (all under build/, never committed):
  build/announcer/takes/<bank>/<clip>.wav   the take you kept for each call
  build/announcer/banks/<bank>/             the built bank, plus <bank>_fxNN.wav as the game will play them
Install copies <bank>.wbk/.ptr/.bfx into <app folder>/announcer/ (default build/appdata2026, the dev
app's folder); the calls load at the app's next start.

The examples are decoded from your ROM on request and never written to disk. Record your own voice
only, or one you have the rights to.
"""
import argparse
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import wave
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "announcer"))
import bank  # noqa: E402
import fs  # noqa: E402

PAGE = os.path.join(ROOT, "tools", "announcer_studio", "index.html")
WORK = os.path.join(ROOT, "build", "announcer")

# The calls a team bank needs (effect id -> what to say). announcer.py build takes them in this order.
CLIPS = [
    {"id": "city", "fx": 0, "label": "City", "hint": "The city alone, short and clipped."},
    {"id": "nickname", "fx": 1, "label": "Nickname", "hint": "The nickname alone. Score calls use it."},
    {"id": "full", "fx": 2, "label": "Full name", "hint": "City and nickname, a confident read. The VS screen says it for the first team."},
    {"id": "full2", "fx": 3, "label": "Full name, hyped", "hint": "The same words with more lift at the end. The VS screen says it for the second team."},
]
TEAMS = [  # new teams record all four; moved teams ("original") keep the game's nickname call unless replaced
    {"bank": "texans", "city": "Houston", "nickname": "Texans"},
    {"bank": "commande", "city": "Washington", "nickname": "Commanders"},
    {"bank": "raiders", "city": "Las Vegas", "nickname": "Raiders", "original": True},
    {"bank": "chargers", "city": "Los Angeles", "nickname": "Chargers", "original": True},
    {"bank": "rams", "city": "Los Angeles", "nickname": "Rams", "original": True, "shares": {"city": "chargers"}},
]
EXAMPLES = [  # team banks to compare with (bank name, how the team is known)
    ("cardinal", "Arizona Cardinals"), ("falcons", "Atlanta Falcons"), ("ravens", "Baltimore Ravens"),
    ("bills", "Buffalo Bills"), ("panthers", "Carolina Panthers"), ("bears", "Chicago Bears"),
    ("bengals", "Cincinnati Bengals"), ("browns", "Cleveland Browns"), ("cowboys", "Dallas Cowboys"),
    ("broncos", "Denver Broncos"), ("lions", "Detroit Lions"), ("packers", "Green Bay Packers"),
    ("colts", "Indianapolis Colts"), ("jaguars", "Jacksonville Jaguars"), ("chiefs", "Kansas City Chiefs"),
    ("dolphins", "Miami Dolphins"), ("vikings", "Minnesota Vikings"), ("patriots", "New England Patriots"),
    ("saints", "New Orleans Saints"), ("giants", "New York Giants"), ("jets", "New York Jets"),
    ("raiders", "Oakland Raiders"), ("eagles", "Philadelphia Eagles"), ("steelers", "Pittsburgh Steelers"),
    ("rams", "St. Louis Rams"), ("chargers", "San Diego Chargers"), ("49ers", "San Francisco 49ers"),
    ("seahawks", "Seattle Seahawks"), ("buccanee", "Tampa Bay Buccaneers"), ("titans", "Tennessee Titans"),
    ("redskins", "Washington"),
]
SAFE = re.compile(r"^[a-z0-9]{1,8}$")


class Studio:
    def __init__(self, rom_path, app_folder):
        self.rom_path = rom_path
        self.app_folder = app_folder
        self.rom = fs.load(rom_path)
        _, self.ents = fs.toc(self.rom)
        self.examples = [(b, n) for b, n in EXAMPLES if bank.load_bank(self.rom, self.ents, b)["ptr"] is not None]
        self.lock = threading.Lock()
        # The original calls' lengths per clip (seconds, average over the teams), to compare takes with.
        self.game_lengths = {}
        for c in CLIPS:
            ls = []
            for b, _ in self.examples:
                bk = bank.load_bank(self.rom, self.ents, b)
                fx = bank.parse_bfx(bk["bfx"])
                ticks = bank.fx_wave_note(fx["effects"][c["fx"]])[2]
                if ticks:
                    ls.append(ticks / 96)
            self.game_lengths[c["id"]] = round(sum(ls) / len(ls), 2) if ls else None

    def take_path(self, team, clip):
        t = next((t for t in TEAMS if t["bank"] == team), {})
        owner = t.get("shares", {}).get(clip, team)  # one "Los Angeles" take serves both LA teams
        return os.path.join(WORK, "takes", owner, f"{clip}.wav")

    def game_call_wav(self, name, fx_id, path):
        """The game's own call for a clip, as a WAV the builder can read (kept under build/)."""
        b = bank.load_bank(self.rom, self.ents, name)
        p, fx = bank.parse_ptr(b["ptr"]), bank.parse_bfx(b["bfx"])
        wv, note, _ = bank.fx_wave_note(fx["effects"][fx_id])
        w = p["waves"][fx["wave_table"][wv]]
        pcm = bank.decode_vadpcm(b["wbk"][w["base"]:w["base"] + w["len"]], w["book"], 2, 4)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(wav_bytes(pcm, bank.OUT_RATE * 2 ** ((w["pitch_offset"] + note) / 12)))
        return path

    def bank_dir(self, team):
        return os.path.join(WORK, "banks", team)

    def state(self):
        teams = []
        for t in TEAMS:
            built = os.path.exists(os.path.join(self.bank_dir(t["bank"]), f"{t['bank']}.wbk"))
            installed = os.path.exists(os.path.join(self.app_folder, "announcer", f"{t['bank']}.wbk"))
            lengths = {}  # as built (trimmed, what the game plays); unbuilt takes aren't measured
            for c in CLIPS:
                path = os.path.join(self.bank_dir(t["bank"]), f"{t['bank']}_fx{c['fx']:02d}.wav")
                if os.path.exists(path):
                    try:
                        with wave.open(path) as w:
                            lengths[c["id"]] = round(w.getnframes() / w.getframerate(), 2)
                    except wave.Error:
                        pass
            teams.append(dict(t, lengths=lengths, saved={c["id"]: os.path.exists(self.take_path(t["bank"], c["id"])) for c in CLIPS},
                              built=built, installed=installed, original=bool(t.get("original")),
                              shares=t.get("shares", {})))
        return {"clips": CLIPS, "teams": teams, "game_lengths": self.game_lengths, "examples": [{"bank": b, "name": n} for b, n in self.examples],
                "rom": os.path.basename(self.rom_path), "app_folder": self.app_folder}

    def example_wav(self, name, fx_id):
        """A game call decoded to a 16-bit WAV at 22050 Hz (browsers play that everywhere)."""
        b = bank.load_bank(self.rom, self.ents, name)
        p, fx = bank.parse_ptr(b["ptr"]), bank.parse_bfx(b["bfx"])
        wv, note, _ = bank.fx_wave_note(fx["effects"][fx_id])
        w = p["waves"][fx["wave_table"][wv]]
        pcm = bank.decode_vadpcm(b["wbk"][w["base"]:w["base"] + w["len"]], w["book"], 2, 4)
        rate = bank.OUT_RATE * 2 ** ((w["pitch_offset"] + note) / 12)  # the effect's note-on adds its note
        return wav_bytes(resample(pcm, rate, 22050), 22050)

    def build(self, team, like):
        t = next(t for t in TEAMS if t["bank"] == team)
        takes = {c["id"]: self.take_path(team, c["id"]) for c in CLIPS}
        if t.get("original") and not os.path.exists(takes["nickname"]):  # keep the game's nickname call
            takes["nickname"] = self.game_call_wav(team, 1, os.path.join(WORK, "game_calls", team, "nickname.wav"))
        missing = [c["label"] for c in CLIPS if not os.path.exists(takes[c["id"]])]
        if missing:
            return {"ok": False, "log": "Still to record: " + ", ".join(missing)}
        out = self.bank_dir(team)
        cmd = [sys.executable, os.path.join(ROOT, "tools", "announcer.py"), "build", team, out,
               "--city", takes["city"], "--nickname", takes["nickname"], "--full", takes["full"],
               "--full2", takes["full2"], "--rom", self.rom_path, "--like", like]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return {"ok": r.returncode == 0, "log": (r.stdout + r.stderr).strip()}

    def install(self, team):
        dest = os.path.join(self.app_folder, "announcer")
        os.makedirs(dest, exist_ok=True)
        for ext in ("wbk", "ptr", "bfx"):
            shutil.copy2(os.path.join(self.bank_dir(team), f"{team}.{ext}"), dest)
        return {"ok": True, "log": f"Installed {team}.wbk/.ptr/.bfx in {dest}. They load at the app's next start."}


def resample(pcm, src, dst):
    if not pcm:
        return []
    n = int(len(pcm) * dst / src)
    out = []
    for i in range(n):
        x = i * src / dst
        j = int(x)
        f = x - j
        a = pcm[j]
        b2 = pcm[j + 1] if j + 1 < len(pcm) else a
        out.append(int(a + (b2 - a) * f))
    return out


def wav_bytes(samples, rate):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(rate))
        w.writeframes(struct.pack("<%dh" % len(samples), *[max(-32768, min(32767, s)) for s in samples]))
    return buf.getvalue()


def read_wav_file(path):
    with wave.open(path) as w:
        return w.getframerate(), list(struct.unpack("<%dh" % w.getnframes(), w.readframes(w.getnframes())))


def make_handler(studio):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def json(self, obj, code=200):
            self.send(code, json.dumps(obj).encode(), "application/json")

        def do_GET(self):
            parts = self.path.split("?")[0].strip("/").split("/")
            try:
                if parts == [""]:
                    self.send(200, open(PAGE, "rb").read(), "text/html; charset=utf-8")
                elif parts == ["api", "state"]:
                    self.json(studio.state())
                elif len(parts) == 4 and parts[:2] == ["api", "example"] and SAFE.match(parts[2]):
                    fx_id = int(parts[3])
                    if not 0 <= fx_id <= 3 or parts[2] not in dict(studio.examples):
                        return self.json({"error": "no such call"}, 404)
                    self.send(200, studio.example_wav(parts[2], fx_id), "audio/wav")
                elif len(parts) == 4 and parts[:2] == ["api", "take"] and SAFE.match(parts[2]):
                    path = studio.take_path(parts[2], parts[3])
                    if parts[3] not in [c["id"] for c in CLIPS] or not os.path.exists(path):
                        return self.json({"error": "no take"}, 404)
                    self.send(200, open(path, "rb").read(), "audio/wav")
                elif len(parts) == 4 and parts[:2] == ["api", "built"] and SAFE.match(parts[2]):
                    fx = {c["id"]: c["fx"] for c in CLIPS}.get(parts[3])
                    path = os.path.join(studio.bank_dir(parts[2]), f"{parts[2]}_fx{fx:02d}.wav") if fx is not None else ""
                    if not path or not os.path.exists(path):
                        return self.json({"error": "not built"}, 404)
                    rate, pcm = read_wav_file(path)
                    self.send(200, wav_bytes(resample(pcm, rate, 22050), 22050), "audio/wav")
                else:
                    self.json({"error": "not found"}, 404)
            except Exception as e:  # report, keep serving
                self.json({"error": str(e)}, 500)

        def do_POST(self):
            parts = self.path.strip("/").split("/")
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length else b""
            try:
                if len(parts) == 4 and parts[:2] == ["api", "take"] and SAFE.match(parts[2]):
                    if parts[3] not in [c["id"] for c in CLIPS] or not body.startswith(b"RIFF"):
                        return self.json({"error": "expected a WAV for a known call"}, 400)
                    path = studio.take_path(parts[2], parts[3])
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    open(path, "wb").write(body)
                    self.json({"ok": True})
                elif len(parts) == 3 and parts[:2] == ["api", "build"] and SAFE.match(parts[2]):
                    like = json.loads(body or b"{}").get("like", "cardinal")
                    if like not in dict(studio.examples):
                        like = "cardinal"
                    with studio.lock:
                        self.json(studio.build(parts[2], like))
                elif len(parts) == 3 and parts[:2] == ["api", "install"] and SAFE.match(parts[2]):
                    with studio.lock:
                        self.json(studio.install(parts[2]))
                else:
                    self.json({"error": "not found"}, 404)
            except Exception as e:
                self.json({"ok": False, "log": str(e)}, 500)

    return Handler


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=os.path.join(ROOT, "build", "baserom.z64"))
    ap.add_argument("--app-folder", default=os.path.join(ROOT, "build", "appdata2026"))
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-open", action="store_true")
    a = ap.parse_args(argv)
    if not os.path.exists(a.rom):
        raise SystemExit(f"No ROM at {a.rom}; pass --rom with your team+ ROM.")
    studio = Studio(a.rom, os.path.abspath(a.app_folder))
    server = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(studio))
    url = f"http://127.0.0.1:{a.port}/"
    print(f"Announcer Studio at {url} (Ctrl-C to stop). Takes and banks go to {WORK}.")
    if not a.no_open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
