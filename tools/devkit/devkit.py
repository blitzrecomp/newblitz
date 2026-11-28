#!/usr/bin/env python3
"""The dev kit: a page in the browser that works a running test game by clicking.

    tools/devkit/devkit.py [--port 8765]      then open http://127.0.0.1:8765

Every button sends one of the game's live commands (src/main/dev_input.cpp: TALL, MARK, BOARD, PLAYART, HINT, CONFIG,
GFX, presses...) and the page shows a fresh screenshot after it. Two targets:

  Mac      the Modern Mac app (make app-modern) through tools/live.sh: commands into build/live/input.txt, screenshots
           of BLITZ DS's two windows stacked as the Duo is held (upper above lower).
  Android  the test device on USB: commands through tools/android/remote.sh (cmd live ...), screenshots with
           adb exec-out screencap (both of the Duo's panels in one).

Local only (127.0.0.1). Nothing it does leaves the machine except what remote.sh already sends to the test mailbox.
"""
import argparse
import io
import json
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
LIVE_INPUT = ROOT / "build/live/input.txt"
LIVE_LOG = ROOT / "build/live/app.log"
LIVE_PID = ROOT / "build/live/pid"
DATA = ROOT / "build/appdata2026"
WINDOWS = ROOT / "build/tools/ds_windows"
ACTIONS = ROOT / "build/live/devkit_actions.log"  # every click: when, what, held or not, the result
LOG_TAGS = ("[tall]", "[board]", "[playart]", "[touch]", "[duo]", "[ds]", "[input]", "[savestate]", "[dev]", "[hint")

held = {"on": False}  # what HOLD was last told, so a page opened (or reloaded) later shows it
job = {"name": "", "lines": [], "running": False}  # the one background job (start, into a game, a harness run)
job_lock = threading.Lock()


def run(cmd, timeout=30, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout, **kw)


def mac_running():
    try:
        os.kill(int(LIVE_PID.read_text().strip()), 0)
        return True
    except Exception:
        return False


def adb_device():
    try:
        out = run(["adb", "devices"], timeout=5).stdout.splitlines()[1:]
        return next((l.split()[0] for l in out if l.strip().endswith("device")), "")
    except Exception:
        return ""


def send(target, line, is_press=False):
    line = line.strip()
    if not line:
        return "empty"
    if target == "android":
        r = run(["tools/android/remote.sh", "cmd", "live", *line.split()], timeout=20)
        return (r.stdout + r.stderr).strip() or "sent"
    if not mac_running():
        return "the Mac test isn't running (Start it first)"
    with open(LIVE_INPUT, "a") as f:
        f.write(line + "\n")
    words = line.split()
    if len(words) >= 2 and words[0].upper() == "HOLD" and words[1] in ("on", "off"):
        held["on"] = words[1] == "on"
    elif held["on"] and words and words[0].upper() not in NO_REFRESH and not is_press:
        refresh_soon()
        return "sent; the held moment redraws"
    return "sent"


# While a moment is held, a change is followed by HOLD refresh: the moment loaded again, run a few frames with it, frozen.
# A burst (a slider dragged) refreshes once, 0.2 s after the last of it.
NO_REFRESH = {"HOLD", "MARK", "SAVESTATE", "LOADSTATE", "DSSTACK", "QUIT", "QUITPROMPT", "RESTART", "DUMP", "PEEK", "HANG"}
refresh_timer = {"t": None}


def refresh_soon():
    def go():
        with open(LIVE_INPUT, "a") as f:
            f.write("HOLD refresh\n")
    if refresh_timer["t"] is not None:
        refresh_timer["t"].cancel()
    refresh_timer["t"] = threading.Timer(0.2, go)
    refresh_timer["t"].start()


def note(kind, line, result):
    with open(ACTIONS, "a") as f:
        f.write(f"{time.strftime('%H:%M:%S')} {kind:5} held={int(held['on'])} {line!r} -> {result}\n")


def press(target, what, hold):
    # Presses are "<BUTTON> <seconds>" lines (tools/live.sh press). On Android, remote.sh's live command takes the same.
    # A held game can't take a press (and a refresh would undo it): let go first, then press.
    if target == "mac" and held["on"]:
        send(target, "HOLD off")
        time.sleep(0.15)
    return send(target, f"{what} {hold}", is_press=True)


def mac_shot():
    if not WINDOWS.exists() or Path(ROOT / "tools/ds_windows.swift").stat().st_mtime > WINDOWS.stat().st_mtime:
        WINDOWS.parent.mkdir(parents=True, exist_ok=True)
        run(["swiftc", "-O", "tools/ds_windows.swift", "-o", str(WINDOWS)], timeout=120)
    pid = LIVE_PID.read_text().strip()
    wins = run([str(WINDOWS), pid], timeout=5).stdout.splitlines()
    up = next((w.split()[0] for w in wins if "upper screen" in w), None)
    low = next((w.split()[0] for w in wins if "upper screen" not in w and "New Blitz" in w), None)
    if low is None:
        return None
    from PIL import Image  # the venv's

    tmp = ROOT / "build/shots/devkit"
    tmp.mkdir(parents=True, exist_ok=True)
    parts = []
    for name, wid in (("up", up), ("low", low)):
        if wid is None:
            continue
        p = tmp / f"_{name}.png"
        run(["screencapture", "-x", "-o", "-l", wid, str(p)], timeout=10)
        if p.exists():
            parts.append(Image.open(p).convert("RGB"))
    if not parts:
        return None
    gap = 24  # the hinge
    w = max(p.width for p in parts)
    img = Image.new("RGB", (w, sum(p.height for p in parts) + gap * (len(parts) - 1)), (0, 0, 0))
    y = 0
    for p in parts:
        img.paste(p, ((w - p.width) // 2, y))
        y += p.height + gap
    img.thumbnail((1000, 1600))
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


def android_shot():
    from PIL import Image

    if adb_device():  # on USB: both panels, the whole screen
        r = subprocess.run(["adb", "exec-out", "screencap", "-p"], cwd=ROOT, capture_output=True, timeout=15)
        if not r.stdout.startswith(b"\x89PNG"):
            return None
        img = Image.open(io.BytesIO(r.stdout)).convert("RGB")
    else:  # any network: the game's own screenshot through the remote mailbox (a few seconds; the game's window only)
        name = "devkit" + time.strftime("%H%M%S")
        out = ROOT / "build/shots/devkit" / f"_{name}.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        run(["tools/android/remote.sh", "cmd", "shot", name], timeout=20)
        run(["tools/android/remote.sh", "res", name, str(out)], timeout=60)
        if not out.exists() or out.stat().st_size == 0:
            return None
        img = Image.open(out).convert("RGB")
    img.thumbnail((1000, 1600))
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


def keep_shot(png, label):
    """Save a shot the page asked to keep, to build/shots/devkit/<time>_<label>.png."""
    d = ROOT / "build/shots/devkit"
    d.mkdir(parents=True, exist_ok=True)
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in label)[:40] or "shot"
    p = d / f"{time.strftime('%H%M%S')}_{safe}.png"
    p.write_bytes(png)
    return str(p.relative_to(ROOT))


def start_job(name, cmd):
    with job_lock:
        if job["running"]:
            return f"busy: {job['name']}"
        job.update(name=name, lines=[f"$ {' '.join(cmd)}"], running=True)

    def work():
        p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in p.stdout:
            with job_lock:
                job["lines"] = (job["lines"] + [line.rstrip()])[-200:]
        p.wait()
        with job_lock:
            job["lines"].append(f"(finished, exit {p.returncode})")
            job["running"] = False

    threading.Thread(target=work, daemon=True).start()
    return f"started: {name}"


def mac_start():
    env = "BLITZ_FLAVOUR=2026"
    return start_job("start", ["bash", "-c", f"{env} tools/live.sh stop >/dev/null 2>&1; {env} tools/live.sh start BLITZ_DS=1 "
                                             "BLITZ_TALL_LOG=1 BLITZ_WINDOW_SIZE=900x675; sleep 20; echo DSSTACK >> build/live/input.txt; "
                                             "echo 'windows stacked'"])


def marks(target):
    if target == "android":
        return []
    d = DATA / "savestates"
    return sorted(p.stem[5:] for p in d.glob("mark-*.state")) if d.exists() else []


def log_tail(target, n=80):
    if target == "android":
        return ["(Android: tools/android/remote.sh cmd log <name> fetches blitz.log)"]
    try:
        with open(LIVE_LOG, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - 400_000))
            lines = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return []
    return [l for l in lines if l.startswith(LOG_TAGS)][-n:]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        target = q.get("target", "mac")
        if u.path in ("/", "/index.html"):
            return self.reply(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
        if u.path == "/api/status":
            with job_lock:
                j = dict(job, lines=job["lines"][-30:])
            return self.reply(200, {"mac": mac_running(), "android": adb_device(), "job": j, "hold": held["on"]})
        if u.path == "/api/shot":
            try:
                png = android_shot() if target == "android" else (mac_shot() if mac_running() else None)
            except Exception as e:
                return self.reply(500, {"error": str(e)})
            if png is None:
                return self.reply(404, {"error": "nothing to shoot"})
            if q.get("keep"):
                self.reply(200, {"saved": keep_shot(png, q.get("keep"))})
                return
            return self.reply(200, png, "image/png")
        if u.path == "/api/marks":
            return self.reply(200, marks(target))
        if u.path == "/api/log":
            return self.reply(200, log_tail(target))
        self.reply(404, {"error": "no such page"})

    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n) or b"{}")
        target = body.get("target", "mac")
        if u.path == "/api/cmd":
            r = send(target, body.get("line", ""))
            note("cmd", body.get("line", ""), r)
            return self.reply(200, {"result": r})
        if u.path == "/api/press":
            r = press(target, body.get("what", "A"), body.get("hold", 0.15))
            note("press", f"{body.get('what')} {body.get('hold')}", r)
            return self.reply(200, {"result": r})
        if u.path == "/api/start":
            return self.reply(200, {"result": mac_start()})
        if u.path == "/api/stop":
            held["on"] = False
            return self.reply(200, {"result": run(["tools/live.sh", "stop"]).stdout.strip()})
        if u.path == "/api/game":  # into an Arcade game, left running (tools/ds_harness.sh --plays 0 --keep)
            cmd = ["tools/ds_harness.sh", "--plays", str(int(body.get("plays", 0))), "--keep"]
            if str(body.get("stadium", "")).isdigit() and int(body["stadium"]) > 0:
                cmd += ["--stadium", str(int(body["stadium"]))]
            if body.get("from"):
                cmd += ["--from", str(body["from"])]
            return self.reply(200, {"result": start_job("into a game", cmd)})
        self.reply(404, {"error": "no such action"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    a = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"dev kit: http://127.0.0.1:{a.port}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
