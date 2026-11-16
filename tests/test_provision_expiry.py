"""The iPhone's "signed until" date (src/main/provision_profile.mm) and install_expiry.h's day counting, on the Mac:
made-up profiles, and every Xcode profile on this machine read without failing (only their dates are printed). macOS only
(Foundation)."""
import glob
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROFILES = os.path.expanduser("~/Library/Developer/Xcode/UserData/Provisioning Profiles/*.mobileprovision")


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("c++"), "needs macOS and a C++ compiler")
class ProvisionExpiry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.tool = pathlib.Path(cls.tmp.name) / "provision_expiry_test"
        subprocess.check_call(["c++", "-std=c++20", "-I", str(ROOT / "src/main"), str(ROOT / "tests/provision_expiry_test.mm"),
                               str(ROOT / "src/main/provision_profile.mm"), "-framework", "Foundation", "-o", str(cls.tool)])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_made_up_profiles(self):
        out = subprocess.run([str(self.tool)], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(out.stdout.strip(), "ok")

    def test_real_profiles(self):
        profiles = sorted(glob.glob(PROFILES))
        if not profiles:
            self.skipTest("no Xcode profiles here")
        out = subprocess.run([str(self.tool), *profiles], capture_output=True, text=True, check=True).stdout
        lines = out.strip().splitlines()
        self.assertEqual(len(lines), len(profiles))
        for line in lines:
            self.assertRegex(line, r"^profile \d+: expires \d{4}-\d\d-\d\d \d\d:\d\d UTC$")


if __name__ == "__main__":
    unittest.main()
