#!/usr/bin/env python3
"""The PSP's roster pack (docs/PSP_PORT.md §6): the weekly rosters' players.json and lineups/*.json as one small text file,
psp/pack.txt, which src/psp/psp_rosters.cpp reads without a JSON tree:

  #blitz-psp-pack/1 <latest week> <updated>
  P|<id>|<first>|<last>|<number>|<pos>|<team>|<heavy>|<skin>     one per player any lineup in the pack uses
  L|<label>|<TEAM>|<id 1>|...|<id 15>                          one per team per lineup ("" = the pack's own player)

<last> is the player file's "name" (else "last"), as rosters_live.cpp shows it; <number> and <skin> are "" when missing.
A lineup id the player file doesn't have is written as "" (the desktop skips it too).

  python3 -I tools/psp/make_pack.py --src build/blitz-rosters --out build/psp/pack.txt
  python3 -I tools/psp/make_pack.py --src build/appdata2026/rosters --hub-dir build/psp/mock_hub   # + manifest.json
  python3 -I tools/psp/make_pack.py --src DIR --snapshot NEWBLITZ/rosters                          # the kit's bundled copy

--src is a folder with players.json and lineups/ (the blitz-rosters repo clone, or an app folder's rosters/). With --hub-dir
(or --snapshot) a manifest.json is written beside psp/pack.txt in the shape the hub serves: the source manifest's fields, its
"files" plus "psp/pack.txt": <sha1>, and "targets": {"psp/pack.txt": {"platforms": ["PSP"]}}. --weeks N keeps the N latest
weeks (default 4) plus "full".
"""
import argparse
import hashlib
import json
import os
import re
import sys

def clean(v):
    return re.sub(r"[|\r\n]", " ", str(v)).strip()


# A player's field as rosters_live.cpp reads it (str_at / int_at): the fallback when it's missing or another type (a null
# name must not become "None", a true number must not become "True").
def str_at(p, key, fallback=""):
    v = p.get(key)
    return v if isinstance(v, str) else fallback


def int_at(p, key, fallback):
    v = p.get(key)
    return int(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else fallback


def ordinal(label):
    m = re.match(r"(\d+)-w(\d+)$", label)
    if m:
        return int(m.group(1)) * 100 + int(m.group(2))
    m = re.match(r"(\d+)-post(\d+)$", label)
    if m:
        return int(m.group(1)) * 100 + 18 + int(m.group(2))
    return 0


def build(src, weeks, latest=None, updated=None):
    """(pack bytes, the source manifest, latest week, players written, lineups kept). latest/updated: the weekly job's
    (it calls this before writing its new manifest); default the source manifest's."""
    players = json.load(open(os.path.join(src, "players.json")))["players"]
    ldir = os.path.join(src, "lineups")
    labels = sorted((f[:-5] for f in os.listdir(ldir) if f.endswith(".json")), key=lambda l: (ordinal(l) == 0, ordinal(l)))
    dated = [l for l in labels if ordinal(l)]
    keep = dated[-weeks:] + (["full"] if "full" in labels else [])
    manifest = {}
    mp = os.path.join(src, "manifest.json")
    if os.path.exists(mp):
        manifest = json.load(open(mp))
    latest = latest or manifest.get("latest") or (dated[-1] if dated else "full")
    updated = updated or manifest.get("updated", "")
    used, lines = [], []
    for label in keep:
        lineup = json.load(open(os.path.join(ldir, label + ".json")))
        for team in sorted(lineup["teams"]):
            ids = [i if isinstance(i, str) and i in players else "" for i in lineup["teams"][team][:15]]
            ids += [""] * (15 - len(ids))
            for i in ids:
                if i and i not in used:
                    used.append(i)
            lines.append("L|%s|%s|%s" % (label, team, "|".join(ids)))
    out = ["#blitz-psp-pack/1 %s %s" % (latest, updated or "-")]
    for i in used:
        p = players[i]
        num = int_at(p, "number", -1)
        skin = int_at(p, "skin", -1)
        out.append("P|%s|%s|%s|%s|%s|%s|%d|%s" % (
            i, clean(str_at(p, "first")), clean(str_at(p, "name", str_at(p, "last"))),
            num if num >= 0 else "", clean(str_at(p, "pos")), clean(str_at(p, "team")),
            int_at(p, "heavy", 0), skin if skin >= 0 else ""))
    text = "\n".join(out + lines) + "\n"
    return text.encode("utf-8"), manifest, latest, len(used), keep


def write(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", help="write the pack here")
    ap.add_argument("--hub-dir", help="write <dir>/psp/pack.txt and <dir>/manifest.json (a hub's layout)")
    ap.add_argument("--snapshot", help="write <dir>/pack.txt and <dir>/manifest.json (the kit's NEWBLITZ/rosters)")
    ap.add_argument("--weeks", type=int, default=4)
    a = ap.parse_args()
    data, manifest, latest, n, keep = build(a.src, a.weeks)
    sha1 = hashlib.sha1(data).hexdigest()
    m = dict(manifest) if manifest else {"format": "blitz-rosters/1"}
    m["format"] = "blitz-rosters/1"
    m["latest"] = latest
    files = dict(m.get("files", {}))
    files["psp/pack.txt"] = sha1
    m["files"] = files
    # Here (a mock hub, the kit's snapshot) only psp/pack.txt gets a target: the desktop's safe_path (rosters_live.cpp) only
    # ever downloads players.json and lineups/, so it skips the pack with or without one. The weekly job (roster_season.py
    # write_manifest, which imports build() from this file) also lists the other platforms on every JSON file.
    targets = dict(m.get("targets", {}))
    targets["psp/pack.txt"] = {"platforms": ["PSP"]}
    m["targets"] = targets
    mtext = (json.dumps(m, indent=1) + "\n").encode()
    if a.out:
        write(a.out, data)
    if a.hub_dir:
        write(os.path.join(a.hub_dir, "psp", "pack.txt"), data)
        write(os.path.join(a.hub_dir, "manifest.json"), mtext)
    if a.snapshot:
        write(os.path.join(a.snapshot, "pack.txt"), data)
        write(os.path.join(a.snapshot, "manifest.json"), mtext)
    print("pack: %s, %d players, lineups %s, %d bytes, sha1 %s" % (latest, n, " ".join(keep), len(data), sha1))


if __name__ == "__main__":
    sys.exit(main())
