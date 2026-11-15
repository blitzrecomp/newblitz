#!/usr/bin/env python3
"""Copy the Blitz '27 design canvas boards into docs/design/redesign/mockups/ so they open from the repo.

Usage: tools/design_mockups_sync.py <folder holding the canvas's project/ files>
       tools/design_mockups_sync.py --shots   (also render shots/*.jpg with headless Chrome; needs the repo served
                                               at http://127.0.0.1:8427, e.g. python3 -m http.server 8427)

The canvas (a Claude Design artifact) names images by upload id (/_blob/<id>); mockups/blob-map.json maps each id
to a file in the repo, and this rewrites them as relative paths. A new upload on the canvas needs a line in the map.
mockups/support.js stands in for the canvas runtime when a board is opened from the repo.
"""
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MOCKUPS = ROOT / "docs" / "design" / "redesign" / "mockups"
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
BASE_URL = "http://127.0.0.1:8427/docs/design/redesign/mockups/"


def sync(src: Path):
    blobs = json.loads((MOCKUPS / "blob-map.json").read_text())
    unknown = set()
    boards = sorted(src.glob("*.dc.html"))
    if not boards:
        sys.exit(f"no .dc.html boards in {src}")
    for f in boards:
        text = f.read_text(encoding="utf-8")

        def local(m):
            if m.group(1) not in blobs:
                unknown.add(m.group(1))
                return m.group(0)
            return blobs[m.group(1)]

        (MOCKUPS / f.name).write_text(re.sub(r"/_blob/([0-9a-f]{32})", local, text), encoding="utf-8")
    shutil.copy(src / "canvas.json", MOCKUPS / "canvas.json")
    print(f"copied {len(boards)} boards to {MOCKUPS.relative_to(ROOT)}")
    if unknown:
        print("upload ids missing from blob-map.json:", ", ".join(sorted(unknown)))
    missing = set()
    for f in MOCKUPS.glob("*.dc.html"):
        for p in re.findall(r"(?:\.\./)+(?:ui|icons|review|assets/promptfont)/[\w@.\-]+\.(?:png|ttf)", f.read_text(encoding="utf-8")):
            if not (MOCKUPS / p).resolve().exists():
                missing.add(p)
    if missing:
        print("referenced files that don't exist:", ", ".join(sorted(missing)))


def shots():
    from PIL import Image  # .venv

    canvas = json.loads((MOCKUPS / "canvas.json").read_text())
    out = MOCKUPS / "shots"
    out.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name, b in canvas["boards"].items():
            png = Path(tmp) / "shot.png"
            # No --virtual-time-budget: it hangs headless Chrome on macOS. The load event is late enough here.
            subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                            "--hide-scrollbars", "--force-device-scale-factor=1", f"--window-size={b['w']},{b['h']}",
                            f"--user-data-dir={tmp}/profile", f"--screenshot={png}", BASE_URL + name],
                           check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            jpg = out / name.replace(".dc.html", ".jpg")
            Image.open(png).convert("RGB").save(jpg, quality=86, optimize=True)
            print("wrote", jpg.relative_to(ROOT))


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--shots" in args:
        args.remove("--shots")
        if args:
            sync(Path(args[0]))
        shots()
    elif args:
        sync(Path(args[0]))
    else:
        sys.exit(__doc__)
