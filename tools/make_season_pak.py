#!/usr/bin/env python3
"""The 2026 Season pak (docs/design/SEASON_PAK.md): New Blitz's licensed content as one zip for the season server.

    make pak-2026          (the Makefile passes the same sources app-modern bundles in a private build)
    tools/make_season_pak.py build --pack packs/blitz2026 --rosters build/blitz-rosters \\
        --houston build/engine32/houston --app-folder build/appdata2026 --out build/paks
    tools/make_season_pak.py check build/paks/season2026.nbpak     (what the app checks, before it's published)

Writes <out>/season2026.nbpak, the same file under its versioned name (season2026-<version>.nbpak, what the server
serves: the name never changes for a version, so it can be cached forever), and <out>/catalog.json with its size and
SHA-256 (tools/publish_pak.sh uploads both). The zip is deterministic: the same content makes the same bytes, so the
catalog's hash only changes when the content does.

Inside, shaped like the app's Resources (src/game/content_dirs.cpp reads it as a second Resources):
    pak.json                      format, id, version, min_build, headshots, every file's SHA-256
    LICENSE-CONTENT.txt           what this is
    packs/blitz2026/              the roster pack (names, numbers, ratings, team order, the 2026 schedule)
    rosters/                      the roster snapshot: manifest.json, players.json, lineups/*.json (no headshots)
    uniforms/<TEAM>/<set>/        set.json and its parts
    art/files/*.wms, art/vs_right/*.wms, announcer/*.wbk|ptr|bfx, probowl/pb_*.wms, houston/houston.json + *.wms
Not inside: the '27 logos and the credits page (ours; the app carries them), headshots, the live-rosters address.
"""
import argparse
import hashlib
import io
import json
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PAK_ID = "season2026"
FORMAT = "blitz-pak/1"
# The oldest app build a pak is for (pak.json min_build): the app reads paks from the build after 593 on, and one
# older than a pak's min_build leaves it unused.
MIN_BUILD = 593
TOP_FOLDERS = {"packs", "rosters", "uniforms", "art", "announcer", "houston", "probowl"}
TOP_FILES = {"pak.json", "LICENSE-CONTENT.txt"}
MAX_PAK = 64 << 20
FIXED_TIME = (1980, 1, 1, 0, 0, 0)  # zip entries carry no build time

# What each part of the app folder contributes (folder -> file extensions), in the app's own terms.
PARTS = {
    "art/files": (".wms",),
    "art/vs_right": (".wms",),
    "uniforms": (".json", ".wms"),
    "announcer": (".wbk", ".ptr", ".bfx"),
    "probowl": (".wms",),
}

LICENSE = """New Blitz: the 2026 Season pak
===============================

An unofficial, fan-made content pack for New Blitz, a recompilation project for a game you own. It is not made,
endorsed or approved by the NFL, the NFLPA, any club, Midway or its successors, or any rights holder.

It holds rosters (player names, numbers and positions as published in public sports data), schedules, and team art,
uniforms and announcer calls drawn by the project's contributors in the style of the 1999 game. Team names and marks
belong to their owners. For personal, non-commercial play only; it is not for sale.

The server that offers this pak can withdraw it at any time; installed copies keep working. To remove it: New Blitz,
Settings, Add-Ons, 2026 Season, Remove.
"""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def collect(args):
    """Every file of the pak: {name in the zip: bytes}."""
    files = {}

    def add_tree(src, dest, exts=None, only=None):
        if not os.path.isdir(src):
            raise SystemExit(f"{src} is missing: the pak can't be made without it")
        for root, dirs, names in os.walk(src):
            dirs[:] = sorted(d for d in dirs if not d.startswith("."))
            for n in sorted(names):
                if n.startswith(".") or (exts and not n.lower().endswith(exts)) or (only and not only(n)):
                    continue
                rel = os.path.relpath(os.path.join(root, n), src).replace(os.sep, "/")
                with open(os.path.join(root, n), "rb") as f:
                    files[f"{dest}/{rel}"] = f.read()

    add_tree(args.pack, "packs/blitz2026")
    for name in ("manifest.json", "players.json"):
        with open(os.path.join(args.rosters, name), "rb") as f:
            files[f"rosters/{name}"] = f.read()
    add_tree(os.path.join(args.rosters, "lineups"), "rosters/lineups", (".json",))
    add_tree(args.houston, "houston", (".json", ".wms"))
    for part in args.parts.split():
        exts = PARTS.get(part)
        if exts is None:
            raise SystemExit(f"unknown part {part} (known: {', '.join(PARTS)})")
        only = (lambda n: n.startswith("pb_")) if part == "probowl" else None
        add_tree(os.path.join(args.app_folder, part), part, exts, only)
    # The app's own art (art/blitz2026/files: the New Blitz 2027 logo, midfield, doors and badges) always shows: the
    # pak's copy of the same game file would win over it (season_pak.h's order), so it's left out (2026-11-14: an
    # earlier art round's NFL shields were covering our doors and VS badges).
    ours = set(os.listdir(os.path.join(ROOT, "art", "blitz2026", "files")))
    for name in [n for n in files if n.startswith("art/files/") and n.split("/")[-1] in ours]:
        del files[name]
    return files


def default_version(files):
    """The roster snapshot's time (2026-11-03T20:31Z -> 2026.11.03.2031): newer rosters, a newer pak."""
    updated = json.loads(files["rosters/manifest.json"]).get("updated", "")
    digits = "".join(c for c in updated if c.isdigit())
    if len(digits) < 12:
        raise SystemExit("rosters/manifest.json has no 'updated' time: pass --version")
    return f"{digits[0:4]}.{digits[4:6]}.{digits[6:8]}.{digits[8:12]}"


def make_zip(files, version, headshots):
    manifest = {
        "format": FORMAT,
        "id": PAK_ID,
        "title": "2026 Season",
        "version": version,
        "min_build": MIN_BUILD,
        "season": 2026,
        "provides": ["rosters", "lineups", "uniforms", "team_art", "vs_helmets", "announcer", "houston", "probowl"],
        # ESPN's headshots aren't in the pak: on means the crew cards may load them from ESPN (src/game/crew.cpp);
        # the server's catalog can still switch them off for everyone.
        "headshots": headshots,
        "files": {},
    }
    files = dict(files)
    files["LICENSE-CONTENT.txt"] = LICENSE.encode()
    for name in sorted(files):
        manifest["files"][name] = sha256(files[name])
    files["pak.json"] = (json.dumps(manifest, indent=1, sort_keys=True) + "\n").encode()
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        for name in ["pak.json"] + sorted(n for n in files if n != "pak.json"):
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, files[name], compresslevel=9)
    return out.getvalue(), manifest


def check(data):
    """What the app checks before it installs (src/game/season_pak.cpp unpack): returns the manifest or raises."""
    if len(data) > MAX_PAK:
        raise ValueError("too big for a season pak")
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        infos = z.infolist()
        if not infos or len(infos) > 8000:
            raise ValueError(f"{len(infos)} entries")
        contents = {}
        for i in infos:
            n = i.filename
            if i.is_dir():
                continue
            parts = n.split("/")
            if (not n or n.startswith("/") or "\\" in n or ":" in n or ".." in parts or "." in parts
                    or n.startswith("__MACOSX") or (len(parts) == 1 and n not in TOP_FILES)
                    or (len(parts) > 1 and parts[0] not in TOP_FOLDERS)):
                raise ValueError(f"a file outside the pak's folders ({n})")
            contents[n] = z.read(i)  # zipfile checks each CRC-32
    manifest = json.loads(contents.get("pak.json", b"null"))
    if not isinstance(manifest, dict) or manifest.get("format") != FORMAT or manifest.get("id") != PAK_ID:
        raise ValueError("not the 2026 Season pak")
    listed = manifest.get("files")
    if not isinstance(listed, dict):
        raise ValueError("pak.json has no file list")
    for n, body in contents.items():
        if n != "pak.json" and listed.get(n) != sha256(body):
            raise ValueError(f"{n} isn't the file pak.json lists")
    missing = set(listed) - set(contents)
    if missing:
        raise ValueError(f"missing {sorted(missing)[0]}")
    pack = json.loads(contents["packs/blitz2026/pack.json"])
    if pack.get("format") != "blitz-pack/2" or pack.get("id") != "blitz2026":
        raise ValueError("the roster pack can't be read")
    return manifest


def catalog(data, manifest, live_rosters, headshots_switch):
    name = f"{PAK_ID}-{manifest['version']}.nbpak"
    return name, {
        "format": "blitz-paks/1",
        "paks": {
            PAK_ID: {
                "title": manifest["title"],
                "version": manifest["version"],
                "file": name,
                "size": len(data),
                "sha256": sha256(data),
                "min_build": manifest["min_build"],
                # The switches every installed copy follows (src/game/season_pak.cpp); removing this entry is the kill
                # switch for new installs (and turns both off).
                "headshots": headshots_switch,
                "live_rosters": live_rosters,
            }
        },
        # Where a pak's install follows the weekly rosters (under the server's address) when it opts in.
        "live_rosters": {"hub": "rosters/"},
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--pack", default="packs/blitz2026")
    b.add_argument("--rosters", default="build/blitz-rosters")
    b.add_argument("--houston", default="build/engine32/houston")
    b.add_argument("--app-folder", default="build/appdata2026")
    b.add_argument("--parts", default=" ".join(PARTS))
    b.add_argument("--out", default="build/paks")
    b.add_argument("--version")
    b.add_argument("--headshots", choices=["on", "off"], default="on", help="pak.json's headshots (default on)")
    b.add_argument("--live-rosters", choices=["on", "off"], default="on", help="the catalog's live_rosters switch")
    c = sub.add_parser("check")
    c.add_argument("pak")
    args = ap.parse_args()

    if args.cmd == "check":
        with open(args.pak, "rb") as f:
            data = f.read()
        try:
            m = check(data)
        except (ValueError, zipfile.BadZipFile, KeyError) as e:
            print(f"{args.pak}: {e}")
            return 1
        print(f"{args.pak}: {m['id']} {m['version']}, {len(m['files'])} files, SHA-256 {sha256(data)}")
        return 0

    files = collect(args)
    data, manifest = make_zip(files, args.version or default_version(files), args.headshots == "on")
    check(data)
    os.makedirs(args.out, exist_ok=True)
    name, cat = catalog(data, manifest, args.live_rosters == "on", True)
    for n in (f"{PAK_ID}.nbpak", name):
        with open(os.path.join(args.out, n), "wb") as f:
            f.write(data)
    with open(os.path.join(args.out, "catalog.json"), "w") as f:
        json.dump(cat, f, indent=1)
        f.write("\n")
    print(f"{os.path.join(args.out, name)}: {len(files) + 2} files, {len(data)} bytes, SHA-256 {sha256(data)}")
    print(f"{os.path.join(args.out, 'catalog.json')}: version {manifest['version']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
