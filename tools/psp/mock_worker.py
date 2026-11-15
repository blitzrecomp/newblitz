#!/usr/bin/env python3
"""A local stand-in for the blitz-logs Worker (tools/blitz_logs_worker/src/index.js) and the rosters hub, for PSP runs that must
not touch the real mailbox (docs/PSP_PORT.md §5.5). Python standard library only. Listens on 127.0.0.1:8788 by default.

Keys are fake and printed on purpose (they are not secrets): upload "mockupload", read "mockread", PSP "mockpsp", hub "mockhub".
Routes, as the Worker has them:
  POST /<upload|psp>/up?device=&kind=[&launch=]   a file; a run log with launch= replaces that launch's copy (same id)
  GET  /<upload>/cmd?pid=      take the "device" mailbox's lines        GET /<psp>/cmd?pid=   the "psp" mailbox's
  POST /<read>/cmd[?box=psp]   queue lines ("@<pid> " targets one pid)  GET /<read>/seen[?box=psp]
  POST, GET /<any>/res/<name>  results; psp-* names live in the psp mailbox, and the PSP key may only touch psp-* names
  GET  /<read>/list, /<read>/get/<id>
  POST /<read>/file/<name>     the file store (NB Remote's updates): 20 MB at most; size and SHA-256 worked out here
  GET  /<read|psp>/file/<name>/meta, /<read|psp>/file/<name>?off=&len= (len 1-256 KB), GET /<read>/files
  GET  /<hub>/manifest.json, /<hub>/psp/pack.txt, /<hub>/players.json, /<hub>/lineups/<x>.json  (from --hub-dir, else a fake)
Uploads are also written to --store (default build/psp/mock_worker/files/<id>) for a look by hand.

  python3 -I tools/psp/mock_worker.py [--port 8788] [--store DIR] [--hub-dir DIR]
"""
import argparse
import hashlib
import json
import os
import re
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

MAX_BYTES = 20 * 1024 * 1024
PSP_MAX_BYTES = 4 * 1024 * 1024
PSP_KINDS = {"run", "run-gz", "crash", "crash-gz", "probe", "logs"}
FILE_MAX = 20 * 1024 * 1024
RANGE_MAX = 256 * 1024
FILE_NAME = re.compile(r"^[A-Za-z0-9_-][A-Za-z0-9._-]{0,59}$")
LAUNCH = re.compile(r"^\d{4}-\d\d-\d\dT\d\d-\d\d-\d\dZ-\d{1,7}$")


def clean(value, fallback):
    out = re.sub(r"[^A-Za-z0-9._-]", "_", value or "")[:60]
    return out or fallback


class Mailbox:
    def __init__(self):
        self.lock = threading.Lock()
        self.queue = ""
        self.polls = {}
        self.seen = {}
        self.res = {}

    def push(self, body):
        with self.lock:
            self.queue += body + "\n"

    def take(self, pid):
        now = time.time() * 1000
        with self.lock:
            self.polls[pid] = now
            for k in [k for k, t in self.polls.items() if now - t > 3600000]:
                del self.polls[k]
            mine, others = [], []
            for raw in self.queue.split("\n"):
                line = raw.rstrip("\r")
                if not line.strip():
                    continue
                m = re.match(r"^@(\S+) (.*)$", line)
                if not m:
                    mine.append(line)
                elif m.group(1) == pid:
                    mine.append(m.group(2))
                elif m.group(1) in self.polls:
                    others.append(line)
            self.queue = "\n".join(others) + "\n" if others else ""
            self.seen = {"at": now, "pid": pid}
            return "\n".join(mine) + "\n" if mine else ""

    def seen_json(self):
        now = time.time() * 1000
        with self.lock:
            ago = {k: now - t for k, t in self.polls.items()}
            out = dict(self.seen)
            out["ago_ms"] = now - self.seen["at"] if self.seen.get("at") else None
            out["polls_ago_ms"] = ago
            return out


class State:
    def __init__(self, args):
        self.args = args
        self.keys = {"upload": args.upload_key, "read": args.read_key, "psp": args.psp_key, "hub": args.hub_key}
        self.boxes = {"device": Mailbox(), "psp": Mailbox()}
        self.files = {}  # id -> (at, device, kind, bytes)
        self.store = {}  # name -> (at_ms, bytes, sha256)
        self.lock = threading.Lock()
        os.makedirs(args.store, exist_ok=True)

    def hub_file(self, rel):
        if self.args.hub_dir:
            p = os.path.join(self.args.hub_dir, rel)
            if os.path.isfile(p):
                return open(p, "rb").read()
            return None
        pack = (b"#blitz-psp-pack/1 10 2026-11-03T12:00:00Z\n"
                b"P|1|Patrick|Mahomes|15|QB|KC|0|2\nL|starters|KC|" + b"|".join([b"1"] * 15) + b"\n")
        if rel == "psp/pack.txt":
            return pack
        if rel == "manifest.json":
            m = {"format": "blitz-rosters/1", "week": 10, "updated": "2026-11-03T12:00:00Z",
                 "files": {"psp/pack.txt": {"sha1": hashlib.sha1(pack).hexdigest(), "bytes": len(pack)}},
                 "targets": {"psp/pack.txt": {"platforms": ["PSP"]}}}
            return json.dumps(m, indent=1).encode()
        return None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = "mock-blitz-logs/1"

    def log_message(self, fmt, *a):
        sys.stderr.write("[mock %s] %s\n" % (datetime.now().strftime("%H:%M:%S"), fmt % a))

    def reply(self, status, body=b"", ctype="text/plain"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def route(self, method):
        st = self.server.state
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        parts = [p for p in url.path.split("/") if p]
        if not parts:
            return self.reply(404, "not found\n")
        k = st.keys
        device, mac, psp = parts[0] == k["upload"], parts[0] == k["read"], parts[0] == k["psp"]
        if parts[0] == k["hub"] and method == "GET" and len(parts) >= 2:
            data = st.hub_file("/".join(parts[1:]))
            return self.reply(200, data, "application/json" if parts[-1].endswith(".json") else "text/plain") if data is not None \
                else self.reply(404, "not found\n")
        if (device or mac or psp) and len(parts) == 3 and parts[1] == "res":
            name = clean(parts[2], "x")
            psp_name = name.startswith("psp-")
            if psp and not psp_name:
                return self.reply(404, "not found\n")
            box = st.boxes["psp" if psp_name and not device else "device"]
            if method == "POST":
                with box.lock:
                    box.res[name] = self.body()
                return self.reply(200, "stored\n")
            with box.lock:
                data = box.res.get(name)
            return self.reply(200, data, "application/octet-stream") if data is not None else self.reply(404, "none yet\n")
        if (device or psp) and len(parts) == 2 and parts[1] == "cmd":
            return self.reply(200, st.boxes["psp" if psp else "device"].take(q.get("pid", "")))
        if (mac or psp) and len(parts) >= 2 and parts[1] == "file":
            name = parts[2] if len(parts) > 2 else ""
            if not FILE_NAME.match(name) or len(parts) > 4 or (len(parts) == 4 and parts[3] != "meta"):
                return self.reply(400, "bad name\n")
            if method == "POST" and len(parts) == 3:
                if not mac:
                    return self.reply(404, "not found\n")
                if int(self.headers.get("Content-Length") or 0) > FILE_MAX:
                    return self.reply(413, "too large\n")
                data = self.body()
                if not data:
                    return self.reply(400, "empty\n")
                meta = (int(time.time() * 1000), data, hashlib.sha256(data).hexdigest())
                with st.lock:
                    st.store[name] = meta
                return self.reply(200, json.dumps({"name": name, "bytes": len(data), "sha256": meta[2], "at": meta[0]}),
                                  "application/json")
            if method != "GET":
                return self.reply(404, "not found\n")
            with st.lock:
                meta = st.store.get(name)
            if meta is None:
                return self.reply(404, "no such file\n")
            if len(parts) == 4:
                return self.reply(200, f"name={name}\nbytes={len(meta[1])}\nsha256={meta[2]}\nat={meta[0]}\n")
            off, ln = q.get("off", ""), q.get("len", "")
            if not re.match(r"^\d{1,9}$", off) or not re.match(r"^\d{1,9}$", ln) or not 1 <= int(ln) <= RANGE_MAX:
                return self.reply(400, f"off and len wanted, len 1-{RANGE_MAX}\n")
            off, ln = int(off), int(ln)
            if off > len(meta[1]):
                return self.reply(416, "off past the end\n")
            if st.args.corrupt_at >= 0 and off <= st.args.corrupt_at < off + ln:   # a test: one flipped byte in transit
                chunk = bytearray(meta[1][off:off + ln])
                chunk[st.args.corrupt_at - off] ^= 0xFF
                return self.reply(200, bytes(chunk), "application/octet-stream")
            return self.reply(200, meta[1][off:off + ln], "application/octet-stream")
        if mac and method == "GET" and len(parts) == 2 and parts[1] == "files":
            with st.lock:
                rows = [{"name": n, "at": m[0], "bytes": len(m[1]), "sha256": m[2]} for n, m in sorted(st.store.items())]
            return self.reply(200, json.dumps(rows), "application/json")
        mac_box = st.boxes["psp" if q.get("box") == "psp" else "device"]
        if mac and len(parts) == 2 and parts[1] == "cmd" and method == "POST":
            mac_box.push(self.body().decode("utf-8", "replace"))
            return self.reply(200, "queued\n")
        if mac and len(parts) == 2 and parts[1] == "seen":
            return self.reply(200, json.dumps(mac_box.seen_json()), "application/json")
        if method == "POST" and len(parts) == 2 and parts[1] == "up" and (device or psp):
            cap = PSP_MAX_BYTES if psp else MAX_BYTES
            if int(self.headers.get("Content-Length") or 0) > cap:
                return self.reply(413, "too large\n")
            data = self.body()
            if not data:
                return self.reply(400, "empty\n")
            stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H-%M-%S-%f")[:-3] + "Z"
            dev = clean(q.get("device"), "device")
            kind = clean(q.get("kind"), "logs")
            if psp:
                if kind not in PSP_KINDS:
                    return self.reply(400, "bad kind\n")
                if not dev.startswith("psp"):
                    dev = clean("psp-" + dev, "psp")
            asked = q.get("launch", "")
            launch = asked if kind.startswith("run") and LAUNCH.match(asked) else ""
            fid = f"{launch}_{dev}_{kind}" if launch else f"{stamp}_{dev}_{kind}_{uuid.uuid4().hex[:8]}"
            with st.lock:
                st.files[fid] = (time.time(), dev, kind, len(data))
            with open(os.path.join(st.args.store, fid), "wb") as f:
                f.write(data)
            return self.reply(200, fid + "\n")
        if method == "GET" and mac and len(parts) == 2 and parts[1] == "list":
            prefix = q.get("prefix", "")
            with st.lock:
                rows = [{"id": i, "bytes": v[3], "device": v[1], "kind": v[2]} for i, v in st.files.items() if i.startswith(prefix)]
            rows.sort(key=lambda r: r["id"], reverse=True)
            return self.reply(200, json.dumps(rows[:1000]), "application/json")
        if method == "GET" and mac and len(parts) == 3 and parts[1] == "get":
            p = os.path.join(st.args.store, clean(parts[2], "x"))
            if os.path.isfile(p):
                return self.reply(200, open(p, "rb").read(), "application/octet-stream")
            return self.reply(404, "no such file\n")
        return self.reply(404, "not found\n")


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8788)
    ap.add_argument("--store", default=os.path.join(root, "build/psp/mock_worker/files"))
    ap.add_argument("--hub-dir", default="")
    ap.add_argument("--upload-key", default="mockupload")
    ap.add_argument("--read-key", default="mockread")
    ap.add_argument("--psp-key", default="mockpsp")
    ap.add_argument("--hub-key", default="mockhub")
    ap.add_argument("--corrupt-at", type=int, default=-1, help="flip the file store byte at this offset in every read (a test)")
    args = ap.parse_args()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.state = State(args)
    sys.stderr.write(f"mock worker on http://{args.host}:{args.port}: upload key {args.upload_key}, read {args.read_key}, "
                     f"psp {args.psp_key}, hub {args.hub_key}; files in {args.store}\n")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
