"""BNSH programs of NVN2 (bnsh_shaders), without game data: the task and mesh shaders in stage slots 7 and 8 past the six NVN stages are read, with their stage from the
control section's stage byte, and words in those slots that are not NVN2 stage records (an NVN program's, or none) are left alone."""
import os
from pathlib import Path
import struct
import sys
import unittest

os.environ.setdefault('DM_GAME', '010015100B514000')
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def ctl(stage, n):
    c = bytearray(0xe0)
    c[:4] = split.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<II', c, 0x40, n, n)
    c[0x56] = stage
    return bytes(c)


def bnsh(slots, size=0x2000, prog_off=0x200):
    """One variation whose program has the given {slot: (stage byte, code byte)} records, and raw words where the value is an int"""
    d = bytearray(size)
    d[:4] = b'BNSH'; d[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', d, 0x7c, 1, 0x100)          # one variation, its records at +0x100
    struct.pack_into('<Q', d, 0x100 + 0x10, prog_off)    # its program at +prog_off
    at = 0x400
    for si, rec in slots.items():
        if isinstance(rec, int):
            struct.pack_into('<Q', d, prog_off + 8 + 8 * si, rec); continue
        stage, b = rec
        code = bytes([b]) * 0x100
        c = ctl(stage, len(code))
        sc, pc, pk = at, at + 0x20, at + 0x100
        d[pc:pc + len(c)] = c; d[pk:pk + len(code)] = code
        struct.pack_into('<QQII', d, sc + 8, pc, pk, len(code), len(c))
        struct.pack_into('<Q', d, prog_off + 8 + 8 * si, sc)
        at += 0x300
    return bytes(d)


class Nvn2SlotTests(unittest.TestCase):
    def test_task_and_mesh_slots(self):
        found = split.bnsh_shaders(bnsh({4: (0, 1), 7: (6, 2), 8: (7, 3)}), 0, 'Shader/x.bfsha')
        self.assertEqual(sorted(st for _, st, _, _ in found), ['ms', 'ps', 'ts'])
        self.assertEqual({st: code[0] for _, st, _, code in found}, {'ps': 1, 'ts': 2, 'ms': 3})
        pairs = split.var_pairs(found)
        self.assertEqual({t[2] for t in pairs if len(t) == 3}, {'mesh', 'task'})

    def test_other_words_left_alone(self):
        """past slot 5, a word that points outside the file or at no NVN2 control section is not a stage record"""
        d = bytearray(bnsh({4: (0, 1), 6: 0x10_0000, 8: 0x40}))
        found = split.bnsh_shaders(bytes(d), 0, 'x')
        self.assertEqual([st for _, st, _, _ in found], ['ps'])

    def test_six_stages_as_before(self):
        found = split.bnsh_shaders(bnsh({0: (0, 4), 4: (0, 5)}), 0, 'x')
        self.assertEqual([st for _, st, _, _ in found], ['vs', 'ps'])

    def test_six_stage_program_at_buffer_end(self):
        """A classical 6-slot program whose 6 stage pointers end right at len(d) does not fail bounds checks for slots 6..8"""
        prog_off = 0x1000 - (8 + 8 * 6)
        found = split.bnsh_shaders(bnsh({0: (0, 4), 4: (0, 5)}, size=0x1000, prog_off=prog_off), 0, 'x')
        self.assertEqual([st for _, st, _, _ in found], ['vs', 'ps'])

    def test_extension_slot_rejects_non_task_mesh_stage(self):
        with self.assertRaises(SystemExit) as cm:
            split.bnsh_shaders(bnsh({7: (5, 2)}), 0, 'x')
        self.assertIn('NVN2 stage slot 7 holds a cs shader', str(cm.exception))


if __name__ == '__main__':
    unittest.main()
