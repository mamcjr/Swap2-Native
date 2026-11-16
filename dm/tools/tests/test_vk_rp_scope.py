"""The Vulkan replay's pass-end barriers (vka_draw.c "Pass-end barriers"; docs/reference/rendering.md "Pass-end barriers scoped to the pass's writes on the Vulkan
replay"), checked in the source: a pass without memory writes ends with the attachment-scoped barrier behind DM_VK_RP_SCOPE, a pass a game barrier ends keeps one full
barrier (DM_VK_BAR_DEDUP), the task/mesh fallback publishes its guest writes and re-records its own barrier after a scoped end, and every transfer, clear or compute
command recorded into the replay's command buffer is followed by a barrier that covers it (the invariant the scoped end relies on). No device, no game inputs."""
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


class PassEnd(unittest.TestCase):
    def setUp(self):
        self.d = src('vka_draw.c')
        self.f = body(self.d, 'void vka_rp_end_at(const char *file, int line) {')

    def test_choice(self):
        f = self.f
        self.assertIn('int full = g_rp_force_full || R.swrite || vka_nan_on() || !VKA_ENV("DM_VK_RP_SCOPE", 1);', f)
        self.assertNotIn('had_clears', f)
        self.assertNotIn('PC.n != 0', f)
        i = f.index('vk.vkCmdEndRendering(g_cb);')
        self.assertLess(i, f.index('int full ='))
        self.assertIn('if (full) { g_rp_full++; vka_barrier(g_cb); }', f)
        self.assertIn('vka_attachment_write_barrier(g_cb);', f)
        self.assertLess(f.index('vka_attachment_write_barrier(g_cb);'), f.index('vka_ub_flush_pass(g_cb)'))   # the kept updates' copies after the pass's barrier
        self.assertLess(f.index('vka_oq_pass_end();'), i)

    def test_helper(self):
        t = src('vka_tex.c')
        h = body(t, 'void vka_attachment_write_barrier_at(VkCommandBuffer cb, const char *file, int line) {')
        self.assertIn('VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT', h)
        self.assertIn('.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT', h)
        self.assertIn('VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,\n'
                      '                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT', h)
        self.assertIn('vka_gp_mark(cb, file, line)', h)
        self.assertNotIn('vka_disp_pend', h)   # not a full barrier: it settles no pending dispatch barrier
        self.assertIn('#define vka_attachment_write_barrier(cb) vka_attachment_write_barrier_at(cb, __FILE_NAME__, __LINE__)', src('vka.h'))

    def test_dedup(self):
        c = body(self.d, 'int vka_rp_end_covers_barrier(void) {')
        self.assertIn('R.on && !vka_nan_on() && VKA_ENV("DM_VK_BAR_DEDUP", 1)', c)
        self.assertIn('g_rp_force_full = 1; vka_rp_end(); g_rp_force_full = 0;', c)
        e = body(src('vka_exec.c'), 'static int exec(const uint8_t *p, int depth) {')
        self.assertIn('if (!bar_kept) bar_covered = vka_rp_end_covers_barrier();', e)
        self.assertIn('case OP_BARRIER: if (g_cb && !bar_kept && !bar_covered) {', e)
        self.assertIn('int bar_covered = 0;', e.split('for (;;) {', 1)[1].split('switch (h.op)', 1)[0])   # reset at every record

    def test_gbar_note(self):
        g = body(src('vka_gbar.c'), 'void vka_gbar_pass_end(VkCommandBuffer cb) {')
        self.assertIn('VKA_ENV("DM_VK_RP_SCOPE_NARROW", 1)', g)   # on by default: off, a scoped end costs game barriers between dispatches their narrowing
        self.assertIn('VKGB_C_FULL', g)
        self.assertIn('vka_gbar_pass_end(g_cb);', self.f.split('else {', 1)[1])   # only after the scoped barrier

    def test_stats(self):
        s = body(self.d, 'void vka_rp_bar_stats(char *b, size_t n, double frames) {')
        for k in ('g_rp_scoped', 'g_rp_full', 'g_rp_scoped_kept', 'g_bar_dedup', 'g_mesh_guest_bar'):
            self.assertIn(k, s)
        self.assertIn('vka_n_attachment_bar - ab0', src('vka_exec.c'))

    def test_live_switches(self):
        m = re.search(r'g_live\[\] = \{(.*?)\};', src('vka_ctl.c'), re.S); assert m, 'no g_live[] table'
        live = m.group(1)
        for k in ('DM_VK_RP_SCOPE', 'DM_VK_RP_SCOPE_NARROW', 'DM_VK_BAR_DEDUP', 'DM_VK_MESH_GUEST_BAR'):
            self.assertIn('"%s"' % k, live)
        tsv = (ROOT / 'docs/env_switches.tsv').read_text(encoding='utf-8')
        for k in ('DM_VK_RP_SCOPE', 'DM_VK_RP_SCOPE_NARROW', 'DM_VK_BAR_DEDUP', 'DM_VK_MESH_GUEST_BAR'):
            self.assertRegex(tsv, r'(?m)^%s\t' % k)

    def test_no_game_gate(self):
        t = src('vka_tex.c'); g = src('vka_gbar.c'); m = src('vka_mesh.inc')
        for f in (self.f, body(self.d, 'int vka_rp_end_covers_barrier(void) {'), body(t, 'void vka_attachment_write_barrier_at('),
                  body(g, 'void vka_gbar_pass_end(VkCommandBuffer cb) {'), body(m, 'static int mesh_compute('), body(m, 'static int mesh_control(')):
            self.assertNotRegex(f, r'DM_GAME|title_id|0400486024DEC000|dm_main_build')


class Mesh(unittest.TestCase):
    def setUp(self):
        self.m = src('vka_mesh.inc')

    def test_guest_writes(self):
        f = body(self.m, 'static int mesh_compute(vka_dstate *d, vka_shader *s, int stage) {')
        i = f.index('vk.vkCmdDispatchIndirect(g_cb, g_mesh_info.buf, g_mesh_info.off')   # (+ 80 for a paired mesh compute)
        tail = f[i:]
        # the full barrier replaces the batch's mesh_barrier (which it covers) instead of following it: one barrier per batch either way
        self.assertIn('if (draw_writes(sh, d) && VKA_ENV("DM_VK_MESH_GUEST_BAR", 1)) { g_mesh_guest_bar++; vka_barrier(g_cb); }\n    else mesh_barrier();', tail)
        self.assertEqual(tail.count('mesh_barrier();'), 1)

    def test_controller_after_scoped_end(self):
        f = body(self.m, 'static int mesh_control(uint32_t phase, uint32_t base, const VkDescriptorBufferInfo *source, int lead) {')
        self.assertIn('(lead && !(was_on && rp_end_was_full()))) mesh_barrier();', f)


# The invariant: every transfer, clear or compute command recorded into g_cb is followed in its function by a barrier that covers it. The exceptions, reviewed: the
# function's caller records that barrier (named with the reason), or the command is followed by a barrier of the fallback's own scope (mesh_barrier) or a pending
# dispatch barrier (vka_disp_pend), which the next non-dispatch GPU work settles with a full barrier.
WORK = re.compile(r'vk\.vkCmd(Copy\w*|FillBuffer|UpdateBuffer|Clear(?:Color|DepthStencil)Image|BlitImage|ResolveImage|Dispatch\w*)\(g_cb\b')
COVER = re.compile(r'vka_barrier\(g_cb\)|mesh_barrier\(\)|vka_transfer_write_barrier\(g_cb\)|vkCmdPipelineBarrier\(g_cb|atomic_exchange\(&vka_disp_pend, g_cb\)')
CALLER = {   # function: why its caller covers it
    'spill_dispatch': 'vka_dispatch records vka_barrier after a spilled dispatch (sb)',
    'mesh_ind_one': 'its callers (vka_mdx_batch, mesh_ind_args) record vka_barrier after it',
    'buf_copy': 'OP_COPY_B2B records vka_barrier after it (vka_exec.c exec)',
    'buf_fill': 'OP_CLEAR_BUF records vka_barrier after it (vka_exec.c exec)',
}


def functions(text):
    """(name, body) of each function defined at column 0, to its matching closing brace (comments and string literals left out of the count)"""
    out = []
    for m in re.finditer(r'(?m)^(?:static |inline )*[A-Za-z_][\w \*]*?\b(\w+)\([^;{]*\)\s*\{', text):
        i, depth = m.end(), 1
        while i < len(text) and depth:
            c = text[i]
            if text.startswith('//', i):
                i = text.find('\n', i)
                i = len(text) if i < 0 else i
                continue
            if text.startswith('/*', i):
                i = text.find('*/', i) + 2
                continue
            if c in '"\'':
                j = i + 1
                while j < len(text) and text[j] != c:
                    j += 2 if text[j] == '\\' else 1
                i = j + 1
                continue
            depth += c == '{'
            depth -= c == '}'
            i += 1
        out.append((m.group(1), text[m.start():i]))
    return out


# vka_tex.c records into a command buffer parameter (the replay's, or the upload batch's: vka_up_cur). Its uploads are leaves of upload() / upload_sparse() / sparse_bind(),
# whose callers record the barrier; reviewed 2026-11-12.
WORK_TEX = re.compile(r'vk\.vkCmd(Copy\w*|FillBuffer|UpdateBuffer|Clear(?:Color|DepthStencil)Image|BlitImage|ResolveImage|Dispatch\w*)\(cb\b')
COVER_TEX = re.compile(r'vka_barrier\(cb\)|vka_transfer_write_barrier\(cb\)|vkCmdPipelineBarrier\(cb')
CALLER_TEX = {
    'upload_rows_tracked': 'vka_tex_prepare, which vka_exec.c follows with vka_barrier(g_cb)',
    'zero_rows': 'upload_sparse_parts',
    'copy_bound': 'upload_sparse_parts',
    'upload_sparse_parts': 'upload_sparse / sparse_bind / upload_: from img_first_use (vka_barrier at its end) or vka_tex_prepare',
    'upload_mt': 'upload_, from upload (upload_tracked / upload_fill): img_first_use or vka_tex_prepare',
    'upload_': 'upload: img_first_use or vka_tex_prepare',
    'bc_copy': 'BC blocks decoded on the CPU: upload_sparse_parts / upload_mt / upload_ (img_first_use or vka_tex_prepare), and vka_exec.c\'s buffer → texture copy, which ends with vka_barrier(g_cb)',
    'vka_tex_writeback': 'the submission end (vka_exec.c), followed there by the ALL_COMMANDS to HOST barrier; the next submission begins with vka_barrier',
}


class Invariant(unittest.TestCase):
    def test_recording_sites(self):
        bad = []
        for name in ('vka_exec.c', 'vka_draw.c', 'vka_mesh.inc', 'vka_tex.c'):
            for fn, b in functions(src(name)):
                for m in WORK.finditer(b):
                    if fn in CALLER or COVER.search(b, m.end()):
                        continue
                    line = b[:m.start()].count('\n')
                    bad.append('%s %s +%d: %s' % (name, fn, line, b[m.start():m.start() + 60]))
        self.assertEqual(bad, [], 'a transfer, clear or compute command into g_cb without a covering barrier after it in its function')

    def test_tex_recording_sites(self):
        bad = []
        for fn, b in functions(src('vka_tex.c')):
            for m in WORK_TEX.finditer(b):
                if fn in CALLER_TEX or COVER_TEX.search(b, m.end()):
                    continue
                bad.append('vka_tex.c %s +%d: %s' % (fn, b[:m.start()].count('\n'), b[m.start():m.start() + 60]))
        self.assertEqual(bad, [], 'a transfer or clear into a vka_tex.c command buffer without a covering barrier after it, or a reviewed caller (CALLER_TEX)')

    def test_tex_callers(self):
        t = src('vka_tex.c')
        f = body(t, 'static void img_first_use(vka_img *im, const tex_t *t, VkCommandBuffer cb, int mode) {')
        for k in ('ST_TRACK', 'ST_SPARSE', 'ST_FILL'):
            self.assertRegex(f, r'if \(mode == %s\) \{[^\n]*if \(\(cb = vka_up_cur\(cb\)\)\) vka_barrier\(cb\); return; \}' % k)
        self.assertTrue(f.endswith('\n    if (cb) vka_barrier(cb);'))   # (body stops before the closing brace)
        e = src('vka_exec.c')
        self.assertIn('vka_tex_prepare(g_cb);\n    vka_barrier(g_cb);', e)
        i = e.index('vka_tex_writeback(g_cb);')
        self.assertIn('vk.vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT', e[i:i + 900])

    def test_callers(self):
        e = src('vka_exec.c')
        self.assertIn('buf_fill(cb->addr, cb->size, cb->value); vka_barrier(g_cb);', e)
        self.assertIn('buf_copy(s_, d, n); vka_barrier(g_cb);', e)
        self.assertIn('if (sb || vka_nan_on() || VKA_ENV("DM_VK_DISPATCH_ORDER", 0)) vka_barrier(g_cb);', src('vka_draw.c'))


if __name__ == '__main__':
    unittest.main()
