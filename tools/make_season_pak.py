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

Any other pak (1.0.6: the Pac-12 and CFL league paks), from its roster pack folder:
    make pak-league ID=pac12 TITLE="Blitz: Pac-12" PACK=packs/pac12 VERSION=2026.11.27
    tools/make_season_pak.py pak --id pac12 --title "Blitz: Pac-12" --version 2026.11.27 --pack packs/pac12 \\
        [--part <folder>=uniforms] [--summary "..."] [--auto off]
writes <out>/<id>-<version>.nbpak and puts its entry into <out>/catalog.json beside the others (the season's stays).
A league pak holds packs/<id>/ (its art in packs/<id>/files, which only plays while the pack does) and, if it must,
uniforms/, art/ or announcer/; never rosters/, houston/ or probowl/ (the season's alone).
"""
import argparse
import hashlib
import io
import json
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PAK_ID = "season2026"
FORMAT = "blitz-pak/1"
# The oldest app build a pak is for (pak.json min_build): the app reads paks from the build after 593 on, and one
# older than a pak's min_build leaves it unused.
MIN_BUILD = 593
TOP_FOLDERS = {"packs", "rosters", "uniforms", "art", "announcer", "houston", "probowl"}
SEASON_FOLDERS = {"rosters", "houston", "probowl"}  # the season's alone (one roster snapshot, one Houston, one Pro Bowl)
TOP_FILES = {"pak.json", "LICENSE-CONTENT.txt"}
ID_RE = re.compile(r"^[a-z0-9_]{1,32}$")  # a pak's id (src/game/content_dirs.cpp valid_pak_id)
# The first app build that reads any pak, not just the season's (1.0.6): an older one never downloads another.
OTHER_MIN_BUILD = 950
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


def check(data, want_id=None):
    """What the app checks before it installs (src/game/season_pak.cpp unpack): returns the manifest or raises.
    Any pak since 1.0.6 (want_id: the one it must be); the season's own folders (rosters, houston, probowl) only in it."""
    if len(data) > MAX_PAK:
        raise ValueError("too big for a pak")
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        infos = z.infolist()
        if not infos or len(infos) > 8000:
            raise ValueError(f"{len(infos)} entries")
        contents = {}
        for i in infos:
            if not i.is_dir():
                contents[i.filename] = z.read(i)  # zipfile checks each CRC-32
    manifest = json.loads(contents.get("pak.json", b"null"))
    if not isinstance(manifest, dict) or manifest.get("format") != FORMAT or not ID_RE.match(str(manifest.get("id", ""))):
        raise ValueError("not a New Blitz pak")
    pak_id = manifest["id"]
    if want_id and pak_id != want_id:
        raise ValueError(f"the {pak_id} pak, not {want_id}")
    folders = TOP_FOLDERS if pak_id == PAK_ID else TOP_FOLDERS - SEASON_FOLDERS
    for n in contents:
        parts = n.split("/")
        if (not n or n.startswith("/") or "\\" in n or ":" in n or ".." in parts or "." in parts
                or n.startswith("__MACOSX") or (len(parts) == 1 and n not in TOP_FILES)
                or (len(parts) > 1 and parts[0] not in folders)):
            raise ValueError(f"a file outside the pak's folders ({n})")
    listed = manifest.get("files")
    if not isinstance(listed, dict):
        raise ValueError("pak.json has no file list")
    for n, body in contents.items():
        if n != "pak.json" and listed.get(n) != sha256(body):
            raise ValueError(f"{n} isn't the file pak.json lists")
    missing = set(listed) - set(contents)
    if missing:
        raise ValueError(f"missing {sorted(missing)[0]}")
    for n, body in contents.items():
        parts = n.split("/")
        if len(parts) == 3 and parts[0] == "packs" and parts[2] == "pack.json":
            pack = json.loads(body)
            if pack.get("format") != "blitz-pack/2" or pack.get("id") != parts[1]:
                raise ValueError(f"the roster pack {parts[1]} can't be read")
    if pak_id == PAK_ID and "packs/blitz2026/pack.json" not in contents:
        raise ValueError("the roster pack can't be read")
    return manifest


def catalog_entry(data, manifest, **extra):
    """A pak's catalog entry (blitz-paks/1; docs/design/SEASON_PAK.md "The catalog")."""
    name = f"{manifest['id']}-{manifest['version']}.nbpak"
    entry = {
        "title": manifest["title"],
        "version": manifest["version"],
        "file": name,
        "size": len(data),
        "sha256": sha256(data),
        "min_build": manifest["min_build"],
    }
    entry.update({k: v for k, v in extra.items() if v is not None})
    return name, entry


def merge_catalog(path, pak_id, entry):
    """The catalog at path with this pak's entry put in (the others kept), so each pak is made on its own."""
    try:
        with open(path) as f:
            cat = json.load(f)
    except (OSError, ValueError):
        cat = {}
    if cat.get("format") != "blitz-paks/1" or not isinstance(cat.get("paks"), dict):
        cat = {"format": "blitz-paks/1", "paks": {}}
    cat["paks"][pak_id] = entry
    cat["paks"] = {k: cat["paks"][k] for k in sorted(cat["paks"], key=lambda k: (k != PAK_ID, k))}
    if pak_id == PAK_ID:
        # Where a pak's install follows the weekly rosters (under the server's address) when it opts in.
        cat["live_rosters"] = {"hub": "rosters/"}
    return cat


def catalog(data, manifest, live_rosters, headshots_switch, path=None):
    # The switches every installed copy follows (src/game/season_pak.cpp); removing this entry is the kill switch for
    # new installs (and turns both off).
    name, entry = catalog_entry(data, manifest, kind="season", headshots=headshots_switch, live_rosters=live_rosters)
    return name, merge_catalog(path or "", PAK_ID, entry)


def make_pak_zip(files, manifest):
    """Any pak: pak.json (manifest plus every file's SHA-256) first, then the files in name order, fixed times."""
    files = dict(files)
    manifest = dict(manifest, files={})
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


def build_other(args):
    """`pak`: a league pak (or any other) from roster pack folders and app-folder parts, into <out> and its catalog."""
    if not ID_RE.match(args.id) or args.id == PAK_ID:
        raise SystemExit(f"{args.id}: a pak id is 1-32 of a-z, 0-9, _ (and not {PAK_ID}: make pak-2026)")
    files = {}
    for pack in args.pack:
        pack_json = os.path.join(pack, "pack.json")
        with open(pack_json, "rb") as f:
            pid = json.load(f).get("id")
        for root, dirs, names in os.walk(pack):
            dirs[:] = sorted(d for d in dirs if not d.startswith("."))
            for n in sorted(names):
                if n.startswith("."):
                    continue
                rel = os.path.relpath(os.path.join(root, n), pack).replace(os.sep, "/")
                with open(os.path.join(root, n), "rb") as f:
                    files[f"packs/{pid}/{rel}"] = f.read()
    for part in args.part or []:  # folder=dest, e.g. build/league_uniforms_out=uniforms
        src, _, dest = part.partition("=")
        if dest.split("/")[0] not in TOP_FOLDERS - SEASON_FOLDERS:
            raise SystemExit(f"{dest}: a league pak's parts go in {sorted(TOP_FOLDERS - SEASON_FOLDERS)}")
        for root, dirs, names in os.walk(src):
            dirs[:] = sorted(d for d in dirs if not d.startswith("."))
            for n in sorted(names):
                if not n.startswith("."):
                    rel = os.path.relpath(os.path.join(root, n), src).replace(os.sep, "/")
                    with open(os.path.join(root, n), "rb") as f:
                        files[f"{dest}/{rel}"] = f.read()
    if not files:
        raise SystemExit("nothing to put in the pak")
    manifest = {"format": FORMAT, "id": args.id, "title": args.title, "version": args.version,
                "min_build": args.min_build, "kind": args.kind}
    if args.summary:
        manifest["summary"] = args.summary
    data, manifest = make_pak_zip(files, manifest)
    check(data, args.id)
    os.makedirs(args.out, exist_ok=True)
    name, entry = catalog_entry(data, manifest, kind=args.kind, summary=args.summary,
                                auto=None if args.auto == "on" else False)
    with open(os.path.join(args.out, name), "wb") as f:
        f.write(data)
    cat_path = os.path.join(args.out, "catalog.json")
    cat = merge_catalog(cat_path, args.id, entry)
    with open(cat_path, "w") as f:
        json.dump(cat, f, indent=1)
        f.write("\n")
    print(f"{os.path.join(args.out, name)}: {len(files) + 1} files, {len(data)} bytes, SHA-256 {sha256(data)}")
    print(f"{cat_path}: {', '.join(cat['paks'])}")
    return 0


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
    o = sub.add_parser("pak", help="a league pak (or any other) from roster pack folders: make pak-league")
    o.add_argument("--id", required=True)
    o.add_argument("--title", required=True)
    o.add_argument("--version", required=True)
    o.add_argument("--pack", action="append", default=[], help="a roster pack folder (pack.json), into packs/<its id>")
    o.add_argument("--part", action="append", help="<folder>=<uniforms|art/files|announcer>: more files")
    o.add_argument("--kind", default="league", help="the catalog's kind (Add-Ons' label), default league")
    o.add_argument("--summary", help="one line for Add-Ons' row")
    o.add_argument("--min-build", type=int, default=OTHER_MIN_BUILD)
    o.add_argument("--auto", choices=["on", "off"], default="on",
                   help="off: offered in Add-Ons (Get, Check) but never downloaded by itself")
    o.add_argument("--out", default="build/paks")
    args = ap.parse_args()

    if args.cmd == "pak":
        return build_other(args)

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
    # Into the catalog that's there: the league paks' entries stay (make pak-league).
    name, cat = catalog(data, manifest, args.live_rosters == "on", True, os.path.join(args.out, "catalog.json"))
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
