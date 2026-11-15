#!/usr/bin/env python3
"""Print the faulting thread of the newest macOS crash report for BlitzRecompiled.

Usage: tools/lastcrash.py [frames]   (reads ~/Library/Logs/DiagnosticReports/BlitzRecompiled-*.ips)
"""
import json
import sys
from pathlib import Path

reports = sorted(Path.home().glob("Library/Logs/DiagnosticReports/BlitzRecompiled-*.ips"), key=lambda p: p.stat().st_mtime)
if not reports:
    sys.exit("no crash reports")
f = reports[-1]
d = json.loads(f.read_text().split("\n", 1)[1])
ex = d.get("exception", {})
print(f.name, ex.get("type"), ex.get("subtype", ""))
t = d["threads"][d["faultingThread"]]
print("thread:", t.get("name") or t.get("queue") or d["faultingThread"])
imgs = d["usedImages"]
for fr in t["frames"][: int(sys.argv[1]) if len(sys.argv) > 1 else 25]:
    img = imgs[fr["imageIndex"]].get("name", "?")
    print(f"  {img:24} {fr.get('symbol', hex(fr.get('imageOffset', 0)))} +{fr.get('symbolLocation', 0)}")
