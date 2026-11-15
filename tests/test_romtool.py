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


if __name__ == "__main__":
    unittest.main()
