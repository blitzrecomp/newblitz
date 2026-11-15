"""The 2026 Season pak (tools/make_season_pak.py, mirroring src/game/season_pak.cpp's checks) and the season server's
routes (tools/rosters_hub_worker, run in Node when it's installed). Synthetic content only."""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import make_season_pak as msp  # noqa: E402


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())


class Fixture:
    """A tiny tree shaped like the real sources: a roster pack, a roster snapshot, Houston and an app folder."""

    def __init__(self, base):
        b = Path(base)
        self.pack, self.rosters, self.houston, self.app = b / "pack", b / "rosters", b / "houston", b / "app"
        write(self.pack / "pack.json", json.dumps({"format": "blitz-pack/2", "id": "blitz2026", "teams": []}))
        write(self.rosters / "manifest.json", json.dumps({"format": "blitz-rosters/1", "updated": "2026-11-03T20:31Z"}))
        write(self.rosters / "players.json", json.dumps({"players": {}}))
        write(self.rosters / "lineups" / "full.json", json.dumps({"teams": {}}))
        write(self.rosters / "README.md", "not in the pak")
        write(self.houston / "houston.json", "{}")
        write(self.houston / "hou_logo.wms", b"wn" + bytes(40))
        write(self.app / "art" / "files" / "chilogo.wms", b"wn" + bytes(40))
        write(self.app / "art" / "files" / "notes.txt", "not art")
        write(self.app / "art" / "vs_right" / "CHI.wms", b"wn" + bytes(40))
        write(self.app / "uniforms" / "DEN" / "01_home_2026" / "set.json", "{}")
        write(self.app / "uniforms" / "DEN" / "01_home_2026" / "den_h0.wms", b"wn" + bytes(40))
        write(self.app / "announcer" / "texans.wbk", b"N64 WaveTables" + bytes(20))
        write(self.app / "probowl" / "pb_lg_l.wms", b"wn" + bytes(40))
        write(self.app / "probowl" / "other.wms", b"wn" + bytes(40))

    def args(self, out, version=None):
        return [sys.executable, str(ROOT / "tools" / "make_season_pak.py"), "build", "--pack", str(self.pack),
                "--rosters", str(self.rosters), "--houston", str(self.houston), "--app-folder", str(self.app),
                "--out", str(out)] + (["--version", version] if version else [])


def rezip(data, edit):
    """The pak's entries through edit({name: bytes}) into a new zip."""
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        files = {i.filename: z.read(i) for i in z.infolist()}
    edit(files)
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        for n, b in files.items():
            z.writestr(n, b)
    return out.getvalue()


class SeasonPak(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.fx = Fixture(self.dir)
        self.out = Path(self.dir) / "out"
        subprocess.run(self.fx.args(self.out), check=True, capture_output=True)
        self.data = (self.out / "season2026.nbpak").read_bytes()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_contents_shaped_like_resources(self):
        with zipfile.ZipFile(io.BytesIO(self.data)) as z:
            names = set(z.namelist())
        self.assertIn("pak.json", names)
        self.assertIn("packs/blitz2026/pack.json", names)
        self.assertIn("rosters/lineups/full.json", names)
        self.assertIn("uniforms/DEN/01_home_2026/den_h0.wms", names)
        self.assertIn("art/vs_right/CHI.wms", names)
        self.assertIn("houston/hou_logo.wms", names)
        self.assertIn("probowl/pb_lg_l.wms", names)
        for left_out in ("rosters/README.md", "art/files/notes.txt", "probowl/other.wms"):
            self.assertNotIn(left_out, names)
        self.assertFalse(any(n.startswith("rosters/faces") or n.endswith(("hub", "token")) for n in names))

    def test_manifest_and_catalog(self):
        m = msp.check(self.data)
        self.assertEqual(m["version"], "2026.11.03.2031")  # from the roster snapshot's time
        self.assertEqual(m["min_build"], msp.MIN_BUILD)
        self.assertTrue(m["headshots"])
        cat = json.loads((self.out / "catalog.json").read_text())
        e = cat["paks"]["season2026"]
        self.assertEqual(e["sha256"], msp.sha256(self.data))
        self.assertEqual(e["size"], len(self.data))
        self.assertEqual((self.out / e["file"]).read_bytes(), self.data)

    def test_deterministic(self):
        out2 = Path(self.dir) / "out2"
        subprocess.run(self.fx.args(out2), check=True, capture_output=True)
        self.assertEqual((out2 / "season2026.nbpak").read_bytes(), self.data)

    def test_rejects(self):
        def refused(edit, fragment):
            with self.assertRaises(ValueError) as cm:
                msp.check(rezip(self.data, edit))
            self.assertIn(fragment, str(cm.exception))

        refused(lambda f: f.__setitem__("../evil.txt", b"x"), "outside")
        refused(lambda f: f.__setitem__("/abs/evil", b"x"), "outside")
        refused(lambda f: f.__setitem__("C:\\evil", b"x"), "outside")
        refused(lambda f: f.__setitem__("art/files/..\\x", b"x"), "outside")
        refused(lambda f: f.__setitem__("bin/run.sh", b"x"), "outside")
        refused(lambda f: f.__setitem__("art/files/extra.wms", b"wn"), "isn't the file")
        refused(lambda f: f.__setitem__("art/files/chilogo.wms", b"changed"), "isn't the file")
        refused(lambda f: f.pop("art/vs_right/CHI.wms"), "missing")

        def other_id(f):
            m = json.loads(f["pak.json"])
            m["id"] = "other"
            f["pak.json"] = json.dumps(m).encode()
        refused(other_id, "not the 2026 Season pak")

    def test_truncated(self):
        with self.assertRaises(zipfile.BadZipFile):
            msp.check(self.data[: len(self.data) // 2])

    def test_check_command(self):
        bad = Path(self.dir) / "bad.nbpak"
        bad.write_bytes(rezip(self.data, lambda f: f.__setitem__("../x", b"x")))
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "make_season_pak.py"), "check", str(bad)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "make_season_pak.py"), "check", str(self.out / "season2026.nbpak")],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)


@unittest.skipUnless(shutil.which("node"), "node isn't installed")
class SeasonServer(unittest.TestCase):
    def test_worker_routes(self):
        worker = ROOT / "tools" / "rosters_hub_worker"
        r = subprocess.run(["node", "--no-warnings", "test/worker_test.mjs"], cwd=worker, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(r.stdout.startswith("ok"), r.stdout)


if __name__ == "__main__":
    unittest.main()
