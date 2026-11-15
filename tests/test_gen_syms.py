"""gen_syms boundary rules on synthetic MIPS words (no game data)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import gen_syms  # noqa: E402

V = 0x80100000
JR_RA, NOP = 0x03E00008, 0
ADDIU_SP = 0x27BDFFE8  # addiu sp, sp, -0x18


def code(*words: int) -> bytes:
    return b"".join(w.to_bytes(4, "big") for w in words)


class GenSymsTest(unittest.TestCase):
    def test_padded_starts(self):
        seg = code(ADDIU_SP, JR_RA, NOP, NOP, NOP, ADDIU_SP, JR_RA, NOP)
        self.assertEqual(gen_syms.padded_starts(seg, V), {V + 0x14})

    def test_looks_like_start(self):
        seg = code(0x24020001, JR_RA, NOP, 0x24020002, 0x24030003, ADDIU_SP)
        self.assertTrue(gen_syms.looks_like_start(seg, 0x0C))   # right after jr ra + delay slot
        self.assertFalse(gen_syms.looks_like_start(seg, 0x10))  # mid-function
        self.assertFalse(gen_syms.looks_like_start(seg, 0x14))  # addiu sp alone: not a boundary
        self.assertTrue(gen_syms.looks_like_start(seg, 0x14, trusted=True))  # a real jal target

    def test_delay_slot_is_never_a_start(self):
        seg = code(ADDIU_SP, JR_RA, 0x27BD0018, 0x24020002)
        self.assertFalse(gen_syms.looks_like_start(seg, 0x08))
        self.assertFalse(gen_syms.looks_like_start(seg, 0x08, trusted=True))
        self.assertTrue(gen_syms.looks_like_start(seg, 0x0C))

    def test_fallthrough_into_body_is_merged(self):
        # jal target is an alignment nop that runs into the body (the audio frame-size bug)
        seg = code(ADDIU_SP, JR_RA, NOP, NOP, 0x00850018, JR_RA, NOP)
        V2 = V + 0x0C
        bounds, merged, unresolved = gen_syms.merge_fallthroughs(seg, V, [(V, V2), (V2, V2 + 4), (V2 + 4, V + 0x1C)], {V2})
        self.assertEqual(bounds, [(V, V2), (V2, V + 0x1C)])
        self.assertEqual(merged, [V2 + 4])
        self.assertEqual(unresolved, [])

    def test_trim_data_islands(self):
        # function, then a word that is not a valid instruction (opcode 0x3B is reserved)
        seg = code(ADDIU_SP, JR_RA, NOP, 0xEC000000, 0xEC000000)
        bounds, islands = gen_syms.trim_data_islands(seg, V, [(V, V + 0x14)])
        self.assertEqual(bounds, [(V, V + 0x0C)])
        self.assertEqual(islands, [(V + 0x0C, V + 0x14)])

    def test_merge_split_function(self):
        # bnez at +0x08 branches to +0x14, which a wrong split put in the next function
        bnez = 0x14400002  # bnez v0, . + 4 + (2 << 2) -> +0x14
        seg = code(ADDIU_SP, NOP, bnez, NOP, NOP, 0x24020001, JR_RA, NOP)
        bounds, merged = gen_syms.merge_split_functions(seg, V, [(V, V + 0x10), (V + 0x10, V + 0x20)], set())
        self.assertEqual(bounds, [(V, V + 0x20)])
        self.assertEqual(merged, [V + 0x10])

    def test_tail_branch_is_not_merged(self):
        b_next = 0x10000002  # b . + 4 + 8 -> +0x10, the next function's start
        seg = code(ADDIU_SP, b_next, NOP, NOP, JR_RA, NOP)
        bounds, merged = gen_syms.merge_split_functions(seg, V, [(V, V + 0x10), (V + 0x10, V + 0x18)], set())
        self.assertEqual(merged, [])


if __name__ == "__main__":
    unittest.main()
