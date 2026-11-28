"""Play art's C++ route walker (src/game/play_art_walk.h) against the research decoder (tools/analysis/play_routes.py).

Builds tests/play_art_test.cpp, runs it over build/baserom.z64 with --dump, and checks every player's raw path in all
45 offense plays against play_routes.py's. Skipped without the ROM or clang++. `make test-play-art` runs it too.
"""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROM = ROOT / "build" / "baserom.z64"
BIN = ROOT / "build" / "tests" / "play_art_test"
sys.path.insert(0, str(ROOT / "tools" / "analysis"))


@unittest.skipUnless(ROM.exists() and shutil.which("clang++"), "needs build/baserom.z64 and clang++")
class PlayArtWalker(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        BIN.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["clang++", "-std=c++20", "-O1", "-Wall", str(ROOT / "tests" / "play_art_test.cpp"), "-o", str(BIN)],
                       check=True)
        run = subprocess.run([str(BIN), str(ROM), "--dump"], capture_output=True, text=True)
        cls.selfcheck = run
        cls.paths = {}
        for line in run.stdout.splitlines():
            parts = line.split()
            play, slot, ending = int(parts[0]), int(parts[1]), parts[2]
            nums = [float(x) for x in parts[3:]]
            cls.paths[(play, slot)] = (ending, list(zip(nums[0::2], nums[1::2])))

    def test_selfchecks(self):
        self.assertEqual(self.selfcheck.returncode, 0, self.selfcheck.stderr)

    def test_matches_play_routes(self):
        import play_routes
        plays = play_routes.load_plays(play_routes.Rom(ROM))
        self.assertEqual(len(plays), 45)
        checked = 0
        for p in plays:
            for pl in p["players"]:
                ending, pts = self.paths[(p["index"], pl["slot"])]
                want = pl["path"]
                self.assertEqual(ending, pl["ending"], f"{p['name']} slot {pl['slot']}")
                self.assertEqual(len(pts), len(want), f"{p['name']} slot {pl['slot']}")
                for (d, l), (wd, wl) in zip(pts, want):
                    self.assertAlmostEqual(d, wd, delta=0.02, msg=f"{p['name']} slot {pl['slot']}")
                    self.assertAlmostEqual(l, wl, delta=0.02, msg=f"{p['name']} slot {pl['slot']}")
                checked += 1
        self.assertEqual(checked, 45 * 7)


if __name__ == "__main__":
    unittest.main()
