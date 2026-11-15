"""The Vulkan replay's dispatch order and narrowed game barriers (vka_draw.c "Dispatch order", vka_gbar.c; docs/reference/rendering.md "A full barrier after every
dispatch on the Vulkan replay" and "A full barrier between the game's compute dispatches on the Vulkan replay"), checked in the source: the barrier after a dispatch is
left pending behind DM_VK_DISPATCH_ORDER, every settle point records it before the host's own GPU work, a full barrier clears it and an image barrier does not, and the
narrowing layer is installed on top of the command encoder. No device, no game inputs."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
VKA = ROOT / 'dm/gfx/vk_android'


def src(name):
    return (VKA / name).read_text(errors='replace')


def body(text, head):
    """the text of the function that starts at head, to its closing brace at column 0"""
    i = text.index(head)
    j = text.index('\n}\n', i)
    return text[i:j]


class Dispatch(unittest.TestCase):
    def setUp(self):
        self.d = src('vka_draw.c')
        self.f = body(self.d, 'void vka_dispatch(vka_dstate *d, const p_dispatch *dp) {')

    def test_no_unconditional_barrier_after_dispatch(self):
        f = self.f
        i = f.index('vk.vkCmdDispatchIndirect(g_cb, ib, io)')
        tail = f[i:]
        self.assertIn('if (sb || vka_nan_on() || VKA_ENV("DM_VK_DISPATCH_ORDER", 0)) vka_barrier(g_cb);', tail)
        self.assertIn('atomic_exchange(&vka_disp_pend, g_cb)', tail)
        for line in tail.splitlines():
            s = line.strip()
            if s.startswith('vka_barrier(g_cb)'):
                self.fail('an unconditional barrier after the dispatch: ' + s)

    def test_indirect_copy_settles(self):
        f = self.f
        i = f.index('if (dp->ind && c0.buf)')
        self.assertLess(f.index('vka_disp_settle(g_cb);', i), f.index('vk.vkCmdCopyBuffer(g_cb, ib, c0.buf', i))

    def test_texstat_settles(self):
        t = body(self.d, 'static void texstat_at(void) {')
        self.assertLess(t.index('vka_rp_end();'), t.index('vka_disp_settle(g_cb);'))
        self.assertLess(t.index('vka_disp_settle(g_cb);'), t.index('vcap_img_copy'))

    def test_settle(self):
        s = body(self.d, 'void vka_disp_settle(VkCommandBuffer cb) {')
        self.assertIn('!= cb) return;', s)
        self.assertIn('vka_barrier(cb);', s)


class Exec(unittest.TestCase):
    def setUp(self):
        self.e = src('vka_exec.c')

    def test_exec_settles_before_chunk_check(self):
        f = body(self.e, 'static int exec(const uint8_t *p, int depth) {')
        i = f.index('vka_disp_settle(g_cb);')
        self.assertLess(i, f.index('chunk_check();'))
        sw = f[f.rindex('switch (h.op)', 0, i):i]
        for op in ('OP_DRAW', 'OP_DRAW_IDX', 'OP_DRAW_TEX', 'OP_CLEAR_COLOR', 'OP_CLEAR_DS', 'OP_CLEAR_TEX', 'OP_CLEAR_BUF', 'OP_COPY_B2B', 'OP_COPY_B2T',
                   'OP_COPY_T2B', 'OP_COPY_T2T', 'OP_UPDATE_UB', 'OP_REPORT_COUNTER'):
            self.assertRegex(sw, r'case %s:' % op)
        self.assertNotIn('case OP_DISPATCH:', sw)
        self.assertIn('vka_gbar_game(g_cb, (uint32_t)bits)', f)

    def test_sub_commit_settles(self):
        f = body(self.e, 'static uint64_t sub_commit(uint64_t reserved, int has_sig, uint64_t signal) {')
        self.assertLess(f.index('vka_rp_end();'), f.index('vka_disp_settle(g_cb);'))
        self.assertLess(f.index('vka_disp_settle(g_cb);'), f.index('vka_tex_writeback(g_cb);'))

    def test_chunk(self):
        f = body(self.e, 'static void chunk(void) {')
        self.assertIn('vka_disp_settle(g_cb);', f.split('VkCommandBuffer next', 1)[0])
        self.assertLess(f.index('vka_gbar_settle(g_cb);'), f.index('vka_up_flush();'))
        self.assertIn('g_cb = next; vka_gbar_cb(g_cb);', f)

    def test_submission_begin(self):
        i = self.e.index('vk.vkBeginCommandBuffer(g_cb, &bi);')
        head = self.e[i:self.e.index('vka_enc_start(g_cb);', i)]
        self.assertIn('atomic_store(&vka_disp_pend, VK_NULL_HANDLE);', head)
        self.assertIn('vka_gbar_cb(g_cb);', head)


class Barriers(unittest.TestCase):
    def test_full_clears_image_does_not(self):
        t = src('vka_tex.c')
        self.assertIn('atomic_compare_exchange_strong(&vka_disp_pend', body(t, 'void vka_barrier_at(VkCommandBuffer cb, const char *file, int line) {'))
        self.assertNotIn('vka_disp_pend', body(t, 'void vka_img_barrier(VkCommandBuffer cb,'))

    def test_live_switches(self):
        c = src('vka_ctl.c')
        live = re.search(r'g_live\[\] = \{(.*?)\};', c, re.S).group(1)
        for k in ('"DM_VK_DISPATCH_ORDER"', '"DM_VK_BAR_NARROW"'):
            self.assertIn(k, live)

    def test_gbar_after_encoder(self):
        d = src('vka_dev.c')
        self.assertLess(d.index('vka_enc_install();'), d.index('vka_gbar_install();'))

    def test_gbar_default_on(self):
        self.assertIn('VKA_ENV("DM_VK_BAR_NARROW", 1)', src('vka_gbar.c'))


if __name__ == '__main__':
    unittest.main()
