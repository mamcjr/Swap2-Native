"""Game barriers kept inside a draw pass on the Vulkan replay (vka_draw.c vka_rp_game_barrier / bar_hazard, vka_tex.c in_fb_or_alias; docs/reference/rendering.md
"Game barriers ending the draw pass on the Vulkan replay"): the alias check's overlap arithmetic as a truth table (after Erick's 5ce3c18), then the source structure:
DM_VK_BAR_KEEP on by default with no game gate, bar_hazard's reasons counted apart, vka_tex_id_in_pass alias-aware while view_of's checks keep image identity.
No device, no game inputs."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
TEX = ROOT / 'dm/gfx/vk_android/vka_tex.c'
DRAW = ROOT / 'dm/gfx/vk_android/vka_draw.c'


def overlap(a, asz, b, bsz):
    """in_fb_or_alias's test, as written in C (unsigned 64-bit, half-open ranges)"""
    return (b - a < asz) if a <= b else (a - b < bsz)


def in_fb_or_alias(im, fbs):
    """im / each fb: (identity, mem or 0, size[, vbind])"""
    vb = lambda x: len(x) > 3 and x[3]
    if any(im[0] == f[0] for f in fbs):
        return 1
    if not im[1] or not im[2]:
        return 1
    for f in fbs:
        if not f[1] or not f[2]:
            return 1
        if vb(im) and vb(f):                 # two residency images may share physical pages at different guest addresses
            return 1
        if overlap(im[1], im[2], f[1], f[2]):
            return 1
    return 0


class Overlap(unittest.TestCase):
    def test_table(self):
        A = 0x10000
        cases = [  # (a, asz, b, bsz, hazard)
            (A, 0x100, A + 0x100, 0x100, 0),     # adjacent after
            (A + 0x100, 0x100, A, 0x100, 0),     # adjacent before
            (A, 0x100, A + 0xff, 0x100, 1),      # one byte over
            (A + 0xff, 0x100, A, 0x100, 1),
            (A, 0x1000, A + 0x10, 0x10, 1),      # contains
            (A + 0x10, 0x10, A, 0x1000, 1),      # contained
            (A, 0x100, A, 0x100, 1),             # the same range
            (A, 0x100, A + 0x1000, 0x100, 0),    # apart
        ]
        for a, asz, b, bsz, want in cases:
            self.assertEqual(int(overlap(a, asz, b, bsz)), want, (hex(a), asz, hex(b), bsz))
            self.assertEqual(int(overlap(b, bsz, a, asz)), want, 'symmetric')

    def test_unknown_is_hazard(self):
        fb = [(1, 0x20000, 0x100)]
        self.assertEqual(in_fb_or_alias((2, 0, 0x100), fb), 1, 'no storage')
        self.assertEqual(in_fb_or_alias((2, 0x30000, 0), fb), 1, 'no size')
        self.assertEqual(in_fb_or_alias((2, 0x30000, 0x100), [(1, 0, 0x100)]), 1, 'a target with no storage')
        self.assertEqual(in_fb_or_alias((2, 0x30000, 0x100), [(1, 0x20000, 0)]), 1, 'a target with no size')
        self.assertEqual(in_fb_or_alias((1, 0, 0), fb), 1, 'the target itself')
        self.assertEqual(in_fb_or_alias((2, 0x30000, 0x100), fb), 0, 'apart')
        self.assertEqual(in_fb_or_alias((2, 0x20080, 0x100), fb), 1, 'over the target')
        self.assertEqual(in_fb_or_alias((2, 0x30000, 0x100, 1), [(1, 0x20000, 0x100, 1)]), 1, 'two residency images apart in guest addresses')
        self.assertEqual(in_fb_or_alias((2, 0x30000, 0x100, 1), fb), 0, 'a residency image against a plain target apart')
        self.assertEqual(in_fb_or_alias((2, 0x30000, 0x100), [(1, 0x20000, 0x100, 1)]), 0, 'a plain texture against a residency target apart')


def body(text, head):
    i = text.index(head)
    return re.sub(r'//[^\n]*', '', text[i:text.index('\n}\n', i)])


class Source(unittest.TestCase):
    def setUp(self):
        self.tex = TEX.read_text(errors='replace')
        self.draw = DRAW.read_text(errors='replace')

    def test_alias_function_matches_model(self):
        f = body(self.tex, 'static int in_fb_or_alias(const vka_img *im) {')
        for s in ('if (in_fb(im)) return 1;', 'if (!im->mem || !im->L.size) return 1;', 'if (!f || !f->mem || !f->L.size) return 1;',
                  'if (im->vbind && f->vbind) return 1;', 'if (a <= b ? b - a < im->L.size : a - b < f->L.size) return 1;'):
            self.assertIn(s, f)

    def test_in_pass_uses_alias(self):
        f = body(self.tex, 'int vka_tex_id_in_pass(int id) {')
        self.assertIn('r = !e || !e->im || in_fb_or_alias(e->im);', f)
        # the view memo and view_of keep image identity (a pass's own target cut to its other levels)
        self.assertNotIn('in_fb_or_alias', self.tex[self.tex.index('typedef struct { int id; uint8_t dim; uint32_t ver, gen; vka_img *im; VkImageView v; } vmemo;'):])

    def test_bar_keep_default_on(self):
        f = body(self.draw, 'int vka_rp_game_barrier(uint32_t bits) {')
        self.assertIn('VKA_ENV("DM_VK_BAR_KEEP", 1)', f)
        self.assertIn('R.swrite', f)
        self.assertNotIn('DM_GAME', f)
        self.assertNotIn('VKA_ENV("DM_VK_BAR_KEEP", 0)', self.draw)

    def test_hazard_reasons(self):
        f = body(self.draw, 'static int bar_hazard(vka_shader *const sh[VKA_NSTAGE], const vka_dstate *d) {')
        for s in ('return BH_WRITES;', 'return BH_WALK;', 'return BH_POOL;', 'return BH_TEX;', 'return BH_NONE;', 'vka_tex_id_in_pass'):
            self.assertIn(s, f)
        self.assertIn('if (bh) { g_bar_late++; g_bar_late_why[bh]++; vka_rp_end(); }', self.draw)
        st = body(self.draw, 'void vka_rp_bar_stats(char *b, size_t n, double frames) {')
        for k in ('BH_WRITES', 'BH_POOL', 'BH_WALK', 'BH_TEX'):
            self.assertIn(f'g_bar_late_why[{k}]', st)

    def test_push_delta_stays_off(self):
        self.assertIn('VKA_ENV("DM_VK_PUSH_DELTA", 0)', self.draw)


if __name__ == '__main__':
    unittest.main()
