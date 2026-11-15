"""wms images (tools/wms.py) and the announcer builder's silence trim (tools/announcer.py). Synthetic data only."""
import math
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "announcer"))

try:
    from PIL import Image
except ImportError:  # the tools need Pillow; skip without it
    Image = None

import announcer  # noqa: E402

if Image is not None:
    import wms  # noqa: E402


def pixels(img):
    return list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())


@unittest.skipIf(Image is None, "Pillow not installed")
class WmsRoundTrip(unittest.TestCase):
    def image(self):
        img = Image.new("RGBA", (13, 7))
        img.putdata([((x * 19) % 256, (y * 37) % 256, (x * y * 11) % 256, 255 if (x + y) % 3 else 0)
                     for y in range(7) for x in range(13)])
        return img

    def test_type8(self):
        data = wms.encode_rgba16(self.image(), 4, 128, 24)
        img, info = wms.decode(data)
        self.assertEqual((info["type"], info["width"], info["height"], info["origin"], info["flags"]), (8, 13, 7, (128, 24), 4))
        for (a, b) in zip(pixels(self.image()), pixels(img)):
            self.assertEqual(a[3] >= 128, b[3] == 255)  # 1-bit alpha
            if a[3] >= 128:
                for c in range(3):
                    self.assertLessEqual(abs(a[c] - b[c]), 8)  # 5 bits per channel

    def test_type19_alpha(self):
        img = self.image()
        img.putdata([p[:3] + ((i * 17) % 256,) for i, p in enumerate(pixels(img))])
        out, info = wms.decode(wms.encode_type19(img, 4, 0, 12))
        self.assertEqual(info["type"], 19)
        for a, b in zip(pixels(img), pixels(out)):
            self.assertLessEqual(abs(a[3] - b[3]), 17)  # 4-bit alpha


class Trim(unittest.TestCase):
    """The silence trim follows the take's own noise floor (a laptop mic's room noise is cut)."""

    def take(self, noise_db, lead_s=0.6, word_s=0.5, tail_s=0.6):
        rnd = random.Random(1)
        rate, noise = announcer.RATE, 10 ** (noise_db / 20)
        quiet = lambda n: [noise * (rnd.random() * 2 - 1) for _ in range(n)]
        word = [0.8 * math.sin(2 * math.pi * 300 * i / rate) for i in range(int(word_s * rate))]
        return quiet(int(lead_s * rate)) + word + quiet(int(tail_s * rate)), len(word)

    def test_quiet_room(self):
        x, n = self.take(-60)
        self.assertLess(len(announcer.trim(x)), n + int(0.1 * announcer.RATE))

    def test_noisy_room(self):
        x, n = self.take(-30)  # above the old fixed -38 dB threshold
        self.assertLess(len(announcer.trim(x)), n + int(0.1 * announcer.RATE))

    def test_keeps_the_word(self):
        x, n = self.take(-30)
        self.assertGreaterEqual(len(announcer.trim(x)), n)


if __name__ == "__main__":
    unittest.main()
