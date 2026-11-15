"""The replay's GIF writer, read back by a small decoder here: every frame decodes to the right number of pixels, and each
pixel is near the colour that went in (median cut's job). Needs a C++ compiler, nothing else."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read_gif(data: bytes):
    """Returns (width, height, [(delay_cs, [r, g, b, ...] for every pixel)])."""
    assert data[:6] == b"GIF89a", data[:6]
    width = data[6] | data[7] << 8
    height = data[8] | data[9] << 8
    assert data[10] & 0x80 == 0  # no global palette
    pos = 13
    frames = []
    delay = 0
    while True:
        kind = data[pos]
        pos += 1
        if kind == 0x3B:
            return width, height, frames
        if kind == 0x21:
            label = data[pos]
            pos += 1
            if label == 0xF9:
                delay = data[pos + 2] | data[pos + 3] << 8
            while data[pos]:
                pos += data[pos] + 1
            pos += 1
            continue
        assert kind == 0x2C, hex(kind)
        w = data[pos + 4] | data[pos + 5] << 8
        h = data[pos + 6] | data[pos + 7] << 8
        flags = data[pos + 8]
        assert flags & 0x80 and (flags & 7) == 7
        pos += 9
        palette = data[pos:pos + 768]
        pos += 768
        min_code = data[pos]
        pos += 1
        stream = bytearray()
        while data[pos]:
            stream += data[pos + 1:pos + 1 + data[pos]]
            pos += data[pos] + 1
        pos += 1
        indices = lzw_decode(bytes(stream), min_code)
        assert len(indices) == w * h, (len(indices), w * h)
        pixels = bytearray()
        for i in indices:
            pixels += palette[i * 3:i * 3 + 3]
        frames.append((delay, bytes(pixels)))


def lzw_decode(stream: bytes, min_code: int) -> bytes:
    clear, end = 1 << min_code, (1 << min_code) + 1
    size = min_code + 1
    table = None
    out = bytearray()
    bits = 0
    count = 0
    prev = None
    for byte in stream:
        bits |= byte << count
        count += 8
        while count >= size:
            code = bits & ((1 << size) - 1)
            bits >>= size
            count -= size
            if code == clear:
                table = [bytes([i]) for i in range(1 << min_code)] + [b"", b""]
                size = min_code + 1
                prev = None
                continue
            if code == end:
                return bytes(out)
            if prev is None:
                entry = table[code]
            else:
                if code < len(table):
                    entry = table[code]
                else:
                    assert code == len(table), (code, len(table))
                    entry = prev + prev[:1]
                if len(table) < 4096:
                    table.append(prev + entry[:1])
            out += entry
            prev = entry
            if len(table) == (1 << size) and size < 12:
                size += 1
    raise AssertionError("no end code")


class ReplayGif(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if cxx is None:
            raise unittest.SkipTest("no C++ compiler")
        cls.tmp = tempfile.TemporaryDirectory()
        tool = pathlib.Path(cls.tmp.name) / "gif_test"
        subprocess.check_call([cxx, "-std=c++20", "-O1", "-I", str(ROOT / "src/main"), str(ROOT / "tests/replay_gif_test.cpp"),
                               str(ROOT / "src/main/replay_gif.cpp"), "-o", str(tool)])
        cls.out = pathlib.Path(cls.tmp.name) / "out"
        subprocess.check_call([str(tool), str(cls.out)])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def frames(self, name):
        return read_gif((self.out / name).read_bytes())

    def test_gradient_is_close(self):
        w, h, frames = self.frames("gradient.gif")
        self.assertEqual((w, h, len(frames)), (480, 270, 6))
        self.assertEqual(frames[0][0], 7)
        for t, (_, px) in enumerate(frames):
            worst = 0
            for y in range(0, h, 17):
                for x in range(0, w, 19):
                    at = (y * w + x) * 3
                    want = (((x + t * 7) * 255 // w) & 255, (y * 255 // h) & 255, (t * 40) & 255)
                    worst = max(worst, max(abs(px[at + k] - want[k]) for k in range(3)))
            self.assertLess(worst, 48)

    def test_noise_fills_the_code_table(self):
        w, h, frames = self.frames("noise.gif")
        self.assertEqual((w, h, len(frames)), (320, 180, 3))

    def test_small_and_tall(self):
        self.assertEqual(self.frames("tiny.gif")[:2], (1, 1))
        self.assertEqual(self.frames("tall.gif")[:2], (180, 270))


if __name__ == "__main__":
    unittest.main()
