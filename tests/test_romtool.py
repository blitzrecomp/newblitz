"""romtool tests on synthetic ROMs (no game data involved)."""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import romtool  # noqa: E402


def fake_z64(size=0x10000) -> bytes:
    b = bytearray(os.urandom(size))
    b[0:4] = bytes.fromhex("80371240")
    b[0x08:0x0C] = (0x80000400).to_bytes(4, "big")
    b[0x20:0x34] = b"SYNTHETIC TEST ROM  "
    b[0x3B:0x3F] = b"NXXE"
    b[0x3F] = 1
    return bytes(b)


def as_v64(z: bytes) -> bytes:
    b = bytearray(z)
    b[0::2], b[1::2] = z[1::2], z[0::2]
    return bytes(b)


def as_n64(z: bytes) -> bytes:
    b = bytearray(z)
    for i in range(0, len(z), 4):
        b[i:i + 4] = z[i:i + 4][::-1]
    return bytes(b)


class RomToolTest(unittest.TestCase):
    def test_byte_orders_normalize(self):
        z = fake_z64()
        for data, fmt in ((z, "z64"), (as_v64(z), "v64"), (as_n64(z), "n64")):
            out, detected = romtool.to_z64(data)
            self.assertEqual(detected, fmt)
            self.assertEqual(out, z)

    def test_rejects_non_rom(self):
        with self.assertRaises(ValueError):
            romtool.to_z64(b"\0" * 0x1000)

    def test_header_fields(self):
        hd = romtool.header(fake_z64())
        self.assertEqual(hd["name"], "SYNTHETIC TEST ROM")
        self.assertEqual(hd["game_code"], "NXXE")
        self.assertEqual(hd["country"], "USA")
        self.assertEqual(hd["entry_point"], "0x80000400")
        self.assertEqual(hd["cic"], "unknown")

    def test_microcode_strings(self):
        b = bytearray(fake_z64())
        tag = b"RSP Gfx ucode F3DEX       fifo 1.23 Yoshitaka Yasumoto 1996 Nintendo.\0"
        b[0x8000:0x8000 + len(tag)] = tag
        self.assertEqual(romtool.microcode_strings(bytes(b)),
                         ["RSP Gfx ucode F3DEX       fifo 1.23 Yoshitaka Yasumoto 1996 Nintendo."])

    def test_known_table_is_consistent(self):
        db = romtool.known()
        md5s = [r["md5"] for r in db["roms"]] + [r["md5"] for r in db["rejected"]]
        self.assertEqual(len(md5s), len(set(md5s)))
        self.assertTrue(all(len(m) == 32 for m in md5s))

    def test_v10_update_patch(self):
        # USA v1.0 updates to v1.1 with the bundled patch: its header names v1.1's SHA-1 (config/target_rom.json's), and
        # it stays small (code bytes only, no assets). Applying it to a real dump: make test-rom-upgrade.
        import struct
        db = romtool.known()
        v10 = next(r for r in db["roms"] if r["id"] == "blitz2000-v10")
        self.assertEqual(v10["updates_to"], "blitz2000-v11")
        patch = (romtool.ROOT / v10["patch"]).read_bytes()
        self.assertLess(len(patch), 64 * 1024)
        magic, old_size, new_size, _, _, new_sha, csize, dsize = struct.unpack_from("<8sIII20s20sII", patch)
        self.assertEqual(magic, b"NBDIFF01")
        self.assertEqual((old_size, new_size), (16 << 20, 16 << 20))
        self.assertEqual(68 + csize + dsize, len(patch))
        self.assertEqual(new_sha.hex(), "3754c8fdf5cdce5de62685ce9be73e06ba5e3577")
        with self.assertRaises(SystemExit):  # not the ROM it's for
            sys.path.insert(0, str(romtool.ROOT / "tools"))
            import make_rom_patch
            make_rom_patch.apply(b"\0" * (16 << 20), patch)


if __name__ == "__main__":
    unittest.main()
