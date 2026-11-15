"""Content-pack validation rules (tools/pack.py, mirroring src/game/content_pack.cpp). Synthetic packs only."""
import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import pack  # noqa: E402

BASE = {
    "format": "blitz-pack/1",
    "id": "test-pack",
    "requires_rom": "blitz2000-v11",
    "teams": [{
        "team": "ARZ",
        "ratings": {"passing": 5},
        "players": [{"slot": 3, "expect": {"last": "Oldname", "number": 16},
                     "first": "New", "last": "Name", "number": 1, "size": 1.1, "announcer_call": "none"}],
    }],
}


def variant(edit):
    p = copy.deepcopy(BASE)
    edit(p)
    return p


class PackValidation(unittest.TestCase):
    def assertInvalid(self, p, fragment):
        with self.assertRaises(pack.PackError) as ctx:
            pack.validate(p)
        self.assertIn(fragment, str(ctx.exception))

    def test_valid(self):
        changes = pack.validate(BASE)
        self.assertIn("team ARZ passing = 5", changes)
        self.assertIn("team ARZ slot 3 number = 1", changes)

    def test_rating_range(self):
        self.assertInvalid(variant(lambda p: p["teams"][0]["ratings"].update(passing=6)), "0-5")
        self.assertInvalid(variant(lambda p: p["teams"][0]["ratings"].update(passing=-1)), "0-5")

    def test_name_length_and_charset(self):
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(last="A" * 16)), "1-15 characters")
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(last="Brady!")), "can't draw")
        pack.validate(variant(lambda p: p["teams"][0]["players"][0].update(last="O'Neil-Smith Jr")))

    def test_number_and_size(self):
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(number=100)), "0-99")
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(size=1.5)), "0.90-1.30")
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(size=True)), "must be a number")

    def test_skin_tones(self):
        # 1 Light and 0 Darker are the game's own; 2-5 are drawn by src/game/skin_tones.cpp.
        for tone in range(6):
            changes = pack.validate(variant(lambda p: p["teams"][0]["players"][0].update(skin=tone)))
            self.assertIn(f"team ARZ slot 3 skin = {tone}", changes)
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(skin=6)), "0-5")
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(skin=-1)), "0-5")
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(body=2)), "0-1")

    def test_teams(self):
        self.assertInvalid(variant(lambda p: p["teams"][0].update(team="HOU")), "unknown team")
        self.assertInvalid(variant(lambda p: p["teams"][0].update(team=31)), "0-30")
        self.assertEqual(pack.team_index("GB"), pack.team_index("G.B"))
        self.assertInvalid(variant(lambda p: p["teams"].append(copy.deepcopy(p["teams"][0]))), "appears twice")

    def test_unknown_keys_and_format(self):
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(speed=99)), "unknown key")
        self.assertInvalid(variant(lambda p: p.update(format="blitz-pack/9")), "format")
        self.assertInvalid(variant(lambda p: p.update(requires_rom="blitz2000-v10")), "requires_rom")
        self.assertInvalid(variant(lambda p: p.update(id="has space")), "\"id\"")

    def test_announcer_call(self):
        # A renamed player can't keep the old name call...
        self.assertInvalid(variant(lambda p: p["teams"][0]["players"][0].update(announcer_call="keep")), "announcer")
        # ...but a player whose name equals the pack's own expectation isn't renamed.
        same = variant(lambda p: p["teams"][0]["players"][0].update(
            first="Old", last="Oldname", announcer_call="keep", expect={"first": "Old", "last": "Oldname"}))
        pack.validate(same)

    def test_city(self):
        ok = variant(lambda p: p["teams"][0].update(city="Arizona", city_abbr="AZ"))
        self.assertIn("team ARZ city = 'Arizona' (team select 'ARIZONA')", pack.validate(ok))
        self.assertInvalid(variant(lambda p: p["teams"][0].update(city="A Very Long City Name")), "city_upper")
        self.assertInvalid(variant(lambda p: p["teams"][0].update(city_upper="X")), "needs \"city\"")


class PackV2Validation(unittest.TestCase):
    BASE = {"format": "blitz-pack/2", "id": "t2", "requires_rom": "blitz2000-teamplus",
            "order": list(pack.FRANCHISES),
            "teams": [{"franchise": "LV", "city": "Las Vegas", "abbr": "LV",
                       "players": [{"slot": 3, "last": "Someone", "number": 12}]}]}

    def bad(self, edit, fragment):
        p = copy.deepcopy(self.BASE)
        edit(p)
        with self.assertRaises(pack.PackError) as ctx:
            pack.validate(p)
        self.assertIn(fragment, str(ctx.exception))

    def test_valid(self):
        self.assertIn("team LV city = 'Las Vegas'", pack.validate(self.BASE))

    def test_order(self):
        self.bad(lambda p: p["order"].pop(), "all 32")
        self.bad(lambda p: p["order"].__setitem__(0, "OAK"), "franchise id")
        self.bad(lambda p: p["order"].__setitem__(1, "ARI"), "twice")

    def test_fields(self):
        self.bad(lambda p: p["teams"][0].update(franchise="HOUS"), "franchise")
        self.bad(lambda p: p["teams"][0].update(city="X" * 16), "1-15")
        self.bad(lambda p: p["teams"][0]["players"][0].update(number=100), "0-99")
        self.bad(lambda p: p.update(requires_rom="blitz2000-v11"), "teamplus")

    def test_skin_tones(self):
        for tone in range(6):
            p = copy.deepcopy(self.BASE)
            p["teams"][0]["players"][0]["skin"] = tone
            pack.validate(p)
        self.bad(lambda p: p["teams"][0]["players"][0].update(skin=6), "0-5")
        self.bad(lambda p: p["teams"][0]["players"][0].update(heavy=2), "0-1")


if __name__ == "__main__":
    unittest.main()
