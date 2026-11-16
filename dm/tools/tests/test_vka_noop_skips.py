"""The Vulkan replay's no-op skips (vka_draw.c; docs/reference/rendering.md "Zero-grid dispatches and zero-count draws on the Vulkan replay"), checked in the source:
a direct dispatch with no workgroups and a direct draw of count 0 return before any pipeline, descriptor or barrier work, behind their switches, without a game gate,
and the dispatch keeps its DM_VK_DISPATCH_LOG / DM_VK_DISPATCH_SKIP index. No device, no game inputs."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / 'dm/gfx/vk_android/vka_draw.c'


def body(text, head):
    """the text of the function that starts at head, to its closing brace at column 0"""
    i = text.index(head)
    j = text.index('\n}\n', i)
    return text[i:j]


class ZeroDispatch(unittest.TestCase):
    def setUp(self):
        self.f = body(SRC.read_text(errors='replace'), 'void vka_dispatch(vka_dstate *d, const p_dispatch *dp) {')

    def test_order(self):
        f = self.f
        cond = '!dp->ind && (dp->x <= 0 || dp->y <= 0 || dp->z <= 0)'
        at = [f.index(s) for s in ('vka_rp_end();', 'di = nd++', cond, 'vka_shader *sh[VKA_NSTAGE]')]
        self.assertEqual(at, sorted(at), 'pass end, then the dispatch index, then the zero-grid return, then the shaders')

    def test_block(self):
        f = self.f
        i = f.index('!dp->ind && (dp->x <= 0')
        blk = f[i:f.index('vka_shader *sh[VKA_NSTAGE]', i)]
        for s in ('!vka_gp_active', '!vka_nan_on()', 'VKA_ENV("DM_VK_ZERO_DISPATCH", 1)', 'd->b.dispatch[0] =', 'd->b.dispatch[1] =', 'd->b.dispatch[2] =',
                  'd->b.nvn2 =', 'g_dispatches++', 'return;'):
            self.assertIn(s, blk)
        for s in ('vka_barrier', 'SKIP', 'DM_GAME_ID'):
            self.assertNotIn(s, blk)

    def test_full_path_kept(self):
        f = self.f
        self.assertIn('vk.vkCmdDispatchIndirect(g_cb, ib, io)', f)
        self.assertIn('else if (dp->x > 0 && dp->y > 0 && dp->z > 0)', f)
        self.assertEqual(f.count('di = nd++'), 1)


class ZeroDraw(unittest.TestCase):
    def setUp(self):
        self.f = body(SRC.read_text(errors='replace'), 'void vka_draw(vka_dstate *d, const p_set_rt *rt, int has_rt,')

    def test_check(self):
        f = self.f
        m = re.search(r'\n\s*if \(([^\n]*VKA_ENV\("DM_VK_ZERO_DRAW", 1\)[^\n]*)\) \{\n([^}]*)\}\n', f)
        assert m is not None, 'a return behind DM_VK_ZERO_DRAW'
        c, body = m.group(1), m.group(2)
        self.assertIn('draw_log(d, NULL, dw, di);', body, 'the draw keeps its DM_VK_DRAW_LOG index')
        self.assertIn('return;', body)
        for s in ('SKIP', 'g_skipped', 'g_draws'):
            self.assertNotIn(s, body)
        for s in ('!mdi', '!g_mesh_pull', '!vka_gp_active', '!(dw ? dw->ind : di->ind)', '(dw ? dw->count : di->count) == 0'):
            self.assertIn(s, c)
        self.assertNotIn('<= 0', c)
        self.assertLess(m.start(), f.index('const int mesh ='), 'before the mesh split')
        self.assertLess(f.index('d->b.base_instance ='), m.start())


if __name__ == '__main__':
    unittest.main()
