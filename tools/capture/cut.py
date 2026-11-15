#!/usr/bin/env python3
"""Cuts trailer clips out of a recorded session (tools/capture/session.sh): every moment the game logged as worth a clip.

    tools/capture/cut.py SESSION_DIR [--all] [--pre 14] [--post 3] [--prores]

SESSION_DIR holds session.mov and session.json (tools/capture/record.swift: the recording's first-frame time) and
events.jsonl (the game's BLITZ_CAPTURE_EVENTS: one line per play's end, one per screenshot). Writes SESSION_DIR/clips/:

    01_Q1-1.23_TD_45yd-pass.mov ...   one clip per moment, 60 fps constant, the game's sound as uncompressed PCM
    clips.md                          what each clip is, its source time, and the play log line

Picked: touchdowns and other scores, turnovers (a pick-six is both), gains of 25 yards or more, and bookmarks (a
screenshot: L3 or R3 + Select, or F12). --all cuts every play. A play's clip runs from --pre seconds before its end (but
not before the previous play ended) to --post after; a bookmark's from 12 s before to 2 s after.
"""
import argparse
import json
import os
import subprocess
import sys


def load(session):
    with open(os.path.join(session, "session.json")) as f:
        rec = json.load(f)
    events = []
    path = os.path.join(session, "events.jsonl")
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        events.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
    return rec, sorted(events, key=lambda e: e["t"])


def duration(movie):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", movie],
                         capture_output=True, text=True)
    try:
        return float(out.stdout.strip())
    except ValueError:
        return 0.0


def tags(e):
    """Why a play is worth a clip, most important first; empty if it isn't."""
    if e.get("kind") == "mark":
        return ["MARK"]
    t = []
    scored = e.get("scored", 0)
    if e.get("turnover") and scored >= 6:
        t.append("PICK-SIX")
    elif e.get("turnover"):
        t.append("TURNOVER")
    if scored >= 6 and "PICK-SIX" not in t:
        t.append("TD")
    elif 0 < scored < 6:
        t.append("SCORE")
    if e.get("gain", 0) >= 25:
        t.append("BIG")
    return t


def slug(s):
    return "".join(c if c.isalnum() else "-" for c in s).strip("-").replace("--", "-")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--all", action="store_true", help="every play, not only the highlights")
    ap.add_argument("--pre", type=float, default=14.0)
    ap.add_argument("--post", type=float, default=3.0)
    ap.add_argument("--prores", action="store_true", help="ProRes 422 HQ instead of high-quality HEVC (big files)")
    a = ap.parse_args()

    movie = os.path.join(a.session, "session.mov")
    rec, events = load(a.session)
    start = rec["start_ms"] / 1000.0
    length = duration(movie)
    if length <= 0:
        raise SystemExit(f"{movie}: no recording")
    out_dir = os.path.join(a.session, "clips")
    os.makedirs(out_dir, exist_ok=True)

    picks = []
    prev_end = None
    for e in events:
        t = e["t"] / 1000.0 - start
        if e.get("kind") == "play":
            why = tags(e)
            if why or a.all:
                lo = t - a.pre
                if prev_end is not None:
                    lo = max(lo, prev_end + 0.5)
                picks.append((max(0.0, lo), min(length, t + a.post), why or ["PLAY"], e))
            prev_end = t
        elif e.get("kind") == "mark":
            picks.append((max(0.0, t - 12.0), min(length, t + 2.0), ["MARK"], e))
        elif e.get("kind") == "shot":  # tools/capture/shoot.sh: a named stretch, from t for "seconds"
            picks.append((max(0.0, t), min(length, t + float(e.get("seconds", 10))), ["SHOT"], e))
    picks = [p for p in picks if p[1] - p[0] >= 1.0]
    if not picks:
        print(f"no moments to cut ({len(events)} events; --all cuts every play)")
        return

    video = (["-c:v", "prores_videotoolbox", "-profile:v", "hq"] if a.prores
             else ["-c:v", "hevc_videotoolbox", "-q:v", "80", "-tag:v", "hvc1"])
    lines = ["# Clips", "", f"From `{movie}` ({length:.0f} s). Times are into the recording.", "",
             "| Clip | Why | From | Play |", "|---|---|---|---|"]
    for i, (lo, hi, why, e) in enumerate(picks, 1):
        if e.get("kind") == "play" and e.get("result", "").startswith("score or special"):
            # The play log's catch-all for a play that ended in a score: name it.
            sc = e.get("scored", 0)
            e = dict(e, result=("touchdown " + ("pass" if e.get("pass") else "run")) if sc >= 6 else
                     "field goal" if sc == 3 else "safety" if sc == 2 else "extra point" if sc == 1 else e["result"])
        if e.get("kind") == "play":
            label = f"{e.get('quarter', '')}-{e.get('clock', '').replace(':', '.')}_{'_'.join(why)}_{slug(e.get('result', ''))}"
            desc = f"{e.get('quarter')} {e.get('clock')} {e.get('score')}: {e.get('result')}"
        elif e.get("kind") == "shot":
            label = e.get("name", "shot")
            desc = e.get("what", "")
        else:
            label = "MARK"
            desc = "bookmark (screenshot)"
        name = f"{label}.mov" if e.get("kind") == "shot" else f"{i:02d}_{label}.mov"
        cmd = ["ffmpeg", "-v", "error", "-y", "-ss", f"{lo:.3f}", "-i", movie, "-t", f"{hi - lo:.3f}",
               "-r", "60", "-fps_mode", "cfr", *video, "-c:a", "pcm_s16le", os.path.join(out_dir, name)]
        r = subprocess.run(cmd)
        status = "" if r.returncode == 0 else " (ffmpeg failed)"
        print(f"{name}  {lo:7.1f}-{hi:7.1f} s{status}")
        lines.append(f"| {name} | {', '.join(why)} | {int(lo // 60)}:{lo % 60:04.1f} | {desc} |")
    with open(os.path.join(out_dir, "clips.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{len(picks)} clips in {out_dir}")


if __name__ == "__main__":
    sys.exit(main())
