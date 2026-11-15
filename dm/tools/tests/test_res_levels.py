# The profile key res_levels (dm/games/game.py; loader.c levels_apply caps a dynamic resolution by its level lists): Pokemon Pokopia's row in the compiled-in header and in
# the runtime profile the one host reads at start (dm/host/rt/game_rt.c's RES_LEVELS rows), the empty default for a profile without the key, and the rows game.py refuses
from pathlib import Path
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import game  # noqa: E402

POKOPIA = '0400A3A01EE4C000'
BUILD = '76a489f3bd3642fa5e18bd83d32c98ba'
ID = '0100000000F0F000'
TSV = ('title_id\t' + ID + '\nname\tTest\nprogram_id\t' + ID + '\nmodules\tmain\nromfs_marker\tromfs.img\n'
       'mem_reserve\t0x100000\nmem_alias\t0x100000\nmem_stack\t0x100000\napp_pool\t0x100000\n')


def synthetic(rows):
    d = Path(tempfile.mkdtemp()) / ID; d.mkdir()
    (d / 'game.tsv').write_text(TSV + rows, encoding='utf-8')
    return game.Game(d)


class ResLevels(unittest.TestCase):
    def test_pokopia_header_and_runtime(self):
        g = game.load(POKOPIA)
        h = game.header(g)
        self.assertIn('#define DM_GAME_NRES_LEVELS 1', h)
        self.assertIn('{"' + BUILD + '", 0x2849658, 34, 9, {{0x2804ff0, 8}, ', h)
        rows = [r.split('\t') for r in game.runtime(g).splitlines() if r.startswith('RES_LEVELS\t')]
        self.assertEqual(rows, [['RES_LEVELS', BUILD, str(0x2849658), '34', *(f'{o}:{n}' for o, n in (
            (0x2804ff0, 8), (0x2805160, 8), (0x2805198, 8), (0x2805458, 8), (0x28055c0, 8), (0x2808b60, 9), (0x2808b69, 10), (0x2808b73, 9), (0x2808b7c, 9)))]])
        self.assertLessEqual(len(rows[0]), 20, 'game_rt.c splits at most 20 columns')
        self.assertNotIn('NRES_LEVELS', game.runtime(g), 'the derived count is not written')

    def test_without_the_key(self):
        g = synthetic('')
        self.assertIn('#define DM_GAME_NRES_LEVELS 0', game.header(g))
        self.assertNotIn('RES_LEVELS', game.runtime(g))

    def test_twelve_lists(self):
        g = synthetic('res_levels\t' + BUILD + '\t0x100\t4\t' + '\t'.join(f'0x{0x200 + i:x}:2' for i in range(12)) + '\n')
        rows = [r.split('\t') for r in game.runtime(g).splitlines() if r.startswith('RES_LEVELS\t')]
        self.assertEqual(len(rows), 1); self.assertEqual(len(rows[0]), 4 + 12)

    def test_rejected(self):
        for rows in ('res_levels\t' + BUILD + '\t0x100\t34\n',                         # no list
                     'res_levels\tnot-a-build\t0x100\t34\t0x200:8\n',
                     'res_levels\t' + BUILD + '\t0x100\t1\t0x200:8\n',                 # a table of one level
                     'res_levels\t' + BUILD + '\t0x100\t34\t0x200:0\n',                # an empty list
                     'res_levels\t' + BUILD + '\t0x100\t34\t0x200\n',                  # no length
                     'res_levels\t' + BUILD + '\t0x100\t34\t' + '\t'.join(['0x200:2'] * 13) + '\n',
                     'res_levels\t' + BUILD + '\t0x100\t34\t0x200:8\ngraphics_scale\t1280x720\t2\thandheld\n'):
            with self.assertRaises(SystemExit, msg=rows):
                game.header(synthetic(rows))


if __name__ == '__main__':
    unittest.main()
