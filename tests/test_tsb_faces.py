"""tsb_faces tests on synthetic NES ROM images (no game data involved).

The builder below writes the layout independently of the reader: pointer tables of 16-bit
little-endian pointers ([jersey BCD][firstLAST] records), faces at 0x3012 + slot offset + team*0x75.
"""
import csv
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import tsb_faces as tf  # noqa: E402

SLOT_OFFS = [0x00, 0x05, 0x0A, 0x0E, 0x12, 0x16, 0x1A, 0x1E, 0x22, 0x26, 0x2A, 0x2E, 0x32, 0x35, 0x38,
             0x3B, 0x3E, 0x41, 0x45, 0x49, 0x4D, 0x51, 0x55, 0x59, 0x5D, 0x61, 0x65, 0x69, 0x6D, 0x71]


def player(team, pos):
    """Deterministic fake player for (rom team slot, position index)."""
    first = "ab"[: 1 + (pos % 2)] + "." * (team % 2)  # "a", "ab", "a.", "ab."
    last = chr(65 + team % 26) + chr(65 + pos % 26) + "X" * (team % 3)
    number = (team + pos) % 100
    face = (team * 7 + pos * 3) % 0xD5
    return first, last, number, face


def to_bcd(n):
    return ((n // 10) << 4) | (n % 10)


def write_table(rom, table_at, data_at, delta, slots):
    """slots: list of (team, pos); writes records from data_at and count+1 pointers (ptr = file - delta)."""
    at = data_at
    for k, (team, pos) in enumerate(slots):
        first, last, number, _ = player(team, pos)
        rec = bytes([to_bcd(number)]) + (first + last).encode()
        rom[at:at + len(rec)] = rec
        ptr = at - delta
        rom[table_at + 2 * k:table_at + 2 * k + 2] = ptr.to_bytes(2, "little")
        at += len(rec)
    end = at - delta
    rom[table_at + 2 * len(slots):table_at + 2 * len(slots) + 2] = end.to_bytes(2, "little")


def build(kind, header=True):
    size = {"nes28": 0x60010, "cxrom105": 0x80010, "cxrom111": 0xC0010, "cxrom111_as_105": 0xC0010}[kind]
    rom = bytearray(b"\xff" * size)
    rom[0:16] = b"NES\x1a\x10\x10\x42" + bytes(9)
    bank0 = -0x8000 + 0x10
    if kind == "nes28":
        write_table(rom, 0x48, 0x6DA, bank0, [(t, p) for t in range(28) for p in range(30)])
        real = list(range(28))
    else:
        rom[0x48] = 0xFF
        write_table(rom, 0x54, 0x6E6, bank0, [(t, p) for t in range(28) for p in range(30)])
        if kind == "cxrom111":
            table, delta = 0x3E054, 0x36010
        else:
            table, delta = 0x3EB0, 0x30010
        write_table(rom, table, delta + 0xC000, delta, [(t, p) for t in range(30, 34) for p in range(30)])
        real = list(range(28)) + list(range(30, 34))
    for t in real:
        block = t - 2 if t >= 30 else t
        for p in range(30):
            rom[0x3012 + SLOT_OFFS[p] + block * 0x75] = player(t, p)[3]
    data = bytes(rom)
    return data if header else data[16:]


def expect(team, pos):
    first, last, number, face = player(team, pos)
    return {"first": first, "last": last, "number": str(number), "face": f"0x{face:02X}",
            "tone": 0 if face < 0x53 else 1}


def pick(players, team_id, pos_name):
    rows = [p for p in players if p["team"] == team_id and p["position"] == pos_name]
    assert len(rows) == 1, (team_id, pos_name, len(rows))
    return {k: rows[0][k] for k in ("first", "last", "number", "face", "tone")}


class CxRom(unittest.TestCase):
    def test_v105_all_players(self):
        layout, players = tf.decode(build("cxrom105"))
        self.assertEqual(layout, "cxrom105")
        self.assertEqual(len(players), 32 * 30)
        self.assertEqual(len({p["team"] for p in players}), 32)
        self.assertNotIn(None, {p["team"] for p in players})
        self.assertTrue(all(p["ok"] for p in players))
        self.assertEqual(pick(players, "BUF", "QB1"), expect(0, 0))
        self.assertEqual(pick(players, "BAL", "WR3"), expect(6, 8))
        self.assertEqual(pick(players, "CAR", "P"), expect(27, 29))   # ends at the 0x6E4 end pointer
        self.assertEqual(pick(players, "SF", "QB1"), expect(30, 0))   # expansion table, face block 28
        self.assertEqual(pick(players, "SEA", "LOLB"), expect(32, 23))
        self.assertEqual(pick(players, "ARI", "P"), expect(33, 29))   # expansion end pointer

    def test_v111_offsets(self):
        layout, players = tf.decode(build("cxrom111"))
        self.assertEqual(layout, "cxrom111")
        self.assertEqual(pick(players, "LAR", "K"), expect(31, 28))

    def test_v111_size_with_v105_offsets_falls_back(self):
        layout, players = tf.decode(build("cxrom111_as_105"))
        self.assertEqual(layout, "cxrom105")
        self.assertEqual(pick(players, "ARI", "QB2"), expect(33, 1))

    def test_headerless(self):
        layout, players = tf.decode(tf.load_rom_bytes(build("cxrom105", header=False)))
        self.assertEqual(pick(players, "NE", "RB1"), expect(2, 2))


class Nes28(unittest.TestCase):
    def test_original_layout(self):
        layout, players = tf.decode(build("nes28"))
        self.assertEqual(layout, "nes28")
        self.assertEqual(len(players), 28 * 30)
        self.assertEqual(pick(players, "TEN", "QB1"), expect(7, 0))   # oilers slot
        self.assertEqual(pick(players, "ATL", "P"), expect(27, 29))   # last record, 0x6D8 end pointer
        self.assertNotIn("CAR", {p["team"] for p in players})


class Basics(unittest.TestCase):
    def test_split_and_bcd_and_tone(self):
        self.assertEqual(tf.split_name(b"joeMONTANA"), ("joe", "MONTANA"))
        self.assertEqual(tf.split_name(b"a.j.BROWN"), ("a.j.", "BROWN"))
        self.assertEqual(tf.split_name(b"qb"), ("q", "b"))  # TSBTool quirk: split at 1 without capitals
        self.assertEqual(tf.bcd(0x87), "87")
        self.assertEqual(tf.bcd(0x1A), "0x1A")
        rom = bytearray(build("cxrom105"))
        rom[0x3012] = 0x52
        rom[0x3012 + 0x05] = 0x53
        _, players = tf.decode(bytes(rom))
        self.assertEqual(pick(players, "BUF", "QB1")["tone"], 0)
        self.assertEqual(pick(players, "BUF", "QB2")["tone"], 1)

    def test_unknown_size_is_an_error(self):
        with self.assertRaises(tf.LayoutError):
            tf.decode(b"NES\x1a" + bytes(0x40000))

    def test_cli_writes_csv(self):
        with tempfile.TemporaryDirectory() as d:
            rom_path, out = Path(d) / "fake.nes", Path(d) / "faces.csv"
            rom_path.write_bytes(build("cxrom105"))
            with redirect_stdout(io.StringIO()):
                tf.main([str(rom_path), "--out", str(out)])
            with open(out, newline="") as fh:
                rows = list(csv.DictReader(fh))
        self.assertEqual(list(rows[0]), ["team", "position", "first", "last", "number", "face", "tone"])
        self.assertEqual(len(rows), 960)
        self.assertEqual(rows[0]["team"], "BUF")
        self.assertEqual(rows[0]["position"], "QB1")

    def test_cli_refuses_garbage(self):
        rom = bytearray(b"\x00" * 0x80010)
        rom[0:4] = b"NES\x1a"
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "junk.nes"
            p.write_bytes(bytes(rom))
            with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as cm:
                tf.main([str(p), "--out", str(Path(d) / "x.csv")])
        self.assertIn("suspicious", str(cm.exception.code))


if __name__ == "__main__":
    unittest.main()
