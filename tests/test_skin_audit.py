"""Skin values from a source's tone scale (tools/skin_audit.py)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import skin_audit  # noqa: E402


class ToneValue(unittest.TestCase):
    def test_threshold(self):
        # The game's own two: 1 Light, 0 Darker.
        self.assertEqual(skin_audit.tone_value(3, threshold=4), 1)
        self.assertEqual(skin_audit.tone_value(4, threshold=4), 0)
        self.assertEqual(skin_audit.tone_value(1, threshold=1), 0)  # a 0/1 source with 1 = darker

    def test_shades(self):
        # Madden's 0-7 scale over Lighter, Light, Light-medium, Medium, Medium-dark, Darker.
        self.assertEqual([skin_audit.tone_value(t, scale_max=7) for t in range(8)], [5, 5, 1, 2, 3, 3, 4, 0])
        self.assertEqual(skin_audit.tone_value(-1, scale_max=7), 5)
        self.assertEqual(skin_audit.tone_value(9, scale_max=7), 0)


if __name__ == "__main__":
    unittest.main()
