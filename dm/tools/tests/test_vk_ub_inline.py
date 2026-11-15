"""The Vulkan replay's inline uniform updates (vka_exec.c "Inline uniform updates"; docs/reference/rendering.md "Inline uniform updates on the Vulkan replay"), checked
in the source: the scoped barrier after the copies, the batch flushed before anything but another update or a state record, and the updates kept in an open pass, copied
at its end after its full barrier, with the draw's hazard check before the images and the pass, behind their switches and without a game gate. No device, no game inputs."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
VK = ROOT / 'dm/gfx/vk_android'


def text(name):
    return (VK / name).read_text(errors='replace')


def body(src, head):
    """the text of the function that starts at head, to its closing brace at column 0"""
    i = src.index(head)
    return src[i:src.index('\n}\n', i)]


def ordered(test, src, *needles):
    at = [src.index(s) for s in needles]
    test.assertEqual(at, sorted(at), ' < '.join(needles))


class Scoped(unittest.TestCase):
    def test_barrier(self):
        f = body(text('vka_tex.c'), 'void vka_transfer_write_barrier_at(')
        self.assertIn('VKA_ENV("DM_VK_UB_SCOPED", 1)', f)
        self.assertIn('vka_barrier_at(cb, file, line)', f, 'the switch off: a full barrier')
        for s in ('VK_ACCESS_TRANSFER_WRITE_BIT', 'VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT', 'VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT'):
            self.assertIn(s, f)

    def test_only_after_the_copies(self):
        src = text('vka_exec.c')
        # after the batch's copies, and with DM_VK_UB_BATCH=0 for an update that staged nothing (the old barrier after every update)
        self.assertEqual(src.count('vka_transfer_write_barrier('), 2)
        self.assertIn('vka_transfer_write_barrier(cb)', body(src, 'static void ub_flush(VkCommandBuffer cb) {'))
        self.assertIn('if (!ub_batch_on) { if (g_inline_ub.n) ub_flush(g_cb); else vka_transfer_write_barrier(g_cb); }', src)


class Batch(unittest.TestCase):
    def setUp(self):
        self.src = text('vka_exec.c')
        self.ex = body(self.src, 'static int exec(const uint8_t *p, int depth) {')

    def test_flush_before_chunk_and_switches(self):
        ordered(self, self.ex, 'if (g_inline_ub.n && h.op != OP_UPDATE_UB && !ub_state_only(h.op) && !ub_list_flow(h.op, depth)) ub_flush(g_cb);', 'chunk_check();', 'vka_state_op(&S.d, h.op, a)')

    def test_list_flow(self):
        f = body(self.src, 'static int ub_list_flow(uint32_t op, int depth) {')
        for s in ('op == OP_BEGIN', 'op == OP_JUMP', 'op == OP_CALL', '(op == OP_END && depth > 0)'):
            self.assertIn(s, f)

    def test_state_only_records_nothing(self):
        f = body(self.src, 'static int ub_state_only(uint32_t op) {')
        ops = set(re.findall(r'case (OP_\w+):', f))
        # every op vka_state_op takes, plus SET_RT / SCISSOR / COPY_STRIDE (exec's own state)
        st = set(re.findall(r'case (OP_\w+)', body(text('vka_draw.c'), 'int vka_state_op(vka_dstate *d, uint32_t op, const void *a) {')))
        self.assertTrue(st <= ops | {'OP_DRAW', 'OP_DRAW_IDX'}, sorted(st - ops))
        for op in ('OP_SET_RT', 'OP_SCISSOR', 'OP_COPY_STRIDE'):
            self.assertIn(op, ops)
        gpu = ('OP_DRAW', 'OP_DRAW_IDX', 'OP_DISPATCH', 'OP_BARRIER', 'OP_CLEAR_COLOR', 'OP_CLEAR_DS', 'OP_CLEAR_TEX', 'OP_CLEAR_BUF', 'OP_COPY_B2B', 'OP_COPY_B2T',
               'OP_COPY_T2B', 'OP_COPY_T2T', 'OP_REPORT_COUNTER', 'OP_RESET_COUNTER', 'OP_SIGNAL_EVENT', 'OP_WAIT_EVENT', 'OP_WAIT_SYNC', 'OP_FENCE_SYNC', 'OP_RENDER_COND',
               'OP_END', 'OP_BEGIN', 'OP_JUMP', 'OP_CALL', 'OP_DRAW_TEX', 'OP_UPDATE_UB')
        for op in gpu:
            self.assertNotIn(op, ops)

    def test_switch_and_safety_flushes(self):
        self.assertIn('VKA_ENV("DM_VK_UB_BATCH", 1)', self.ex)
        self.assertIn('if (!ub_batch_on) { if (g_inline_ub.n) ub_flush(g_cb);', self.ex)
        self.assertIn('ub_flush(g_cb); VRED("command record of size', self.ex)
        sc = body(self.src, 'static uint64_t sub_commit(uint64_t reserved, int has_sig, uint64_t signal) {')
        ordered(self, sc, 'vka_rp_end();', 'if (g_inline_ub.n)', 'vka_tex_writeback(g_cb);', 'VK_PIPELINE_STAGE_HOST_BIT', 'vkEndCommandBuffer')
        ck = body(self.src, 'static void chunk(void) {')
        ordered(self, ck, 'if (g_inline_ub.n)', 'vka_up_flush();', 'part_submit')

    def test_overlap_flush(self):
        # the rule itself is vka_ubq.h's ubq_must_flush, checked by dm/host/tests/t_android_ubq.c
        f = body(self.src, 'static void ub_add(')
        self.assertIn('ubq_must_flush(b->r, b->n, UB_BATCH_MAX, b->src == sb && b->dst == db, dof, k)', f)


class Defer(unittest.TestCase):
    def setUp(self):
        self.ex_src = text('vka_exec.c')
        self.draw = text('vka_draw.c')

    def test_flush_after_full_barrier(self):
        f = body(self.draw, 'void vka_rp_end(void) {')
        ordered(self, f, 'vk.vkCmdEndRendering(g_cb);', 'vka_barrier(g_cb);', 'vka_ub_flush_pass(g_cb);', 'R.on = 0;')

    def test_no_second_barrier_in_flush(self):
        f = body(self.ex_src, 'void vka_ub_flush_pass(VkCommandBuffer cb) {')
        self.assertNotIn('vka_barrier(', f)
        ordered(self, f, 'vka_tex_gw_update(w->addr, w->size, w->s.ptr)', 'ub_copy(cb, &w->s, w->addr, w->size)', '}\n    ub_flush(cb);', 'g_ub_np = 0; g_ub_seq++;')

    def test_hazard_before_images_and_pass(self):
        f = body(self.draw, 'void vka_draw(vka_dstate *d, const p_set_rt *rt, int has_rt,')
        ordered(self, f, 'walk_all(d, sh);', 'ub_hazard(d, sh, dw, di, mesh)', 'if (make_images(d, sh, 1))', 'if (!rp_begin(rt, has_rt, d))')
        ordered(self, f, 'if (R.on && vka_ub_active()) { vka_ub_hazard_end(); vka_rp_end(); }', 'mesh_emulate(')

    def test_hazard_ranges(self):
        f = body(self.draw, 'static int ub_hazard(')
        self.assertIn('VKA_ENV("DM_VK_UB_DEFER_IDX", 1)', f)
        self.assertNotIn('mdi_i', f)
        self.assertIn('ubq_index_len(di->itype, di->count, ind || mdi)', f)
        self.assertIn('ubq_mdi_len(mx, stride, rec)', f)
        for s in ('g_wk.pool[st]', 'VKA_R_GMEM_MAP', 'VKA_R_TBUF', 'vka_tex_id_overlaps_pending_ub', 'vka_tex_gw_shadow_safe', 'DMSH_VK_BF_RW'):
            self.assertIn(s, f)

    def test_explicit_first_switch_case_and_default_off(self):
        ex = body(self.ex_src, 'static int exec(const uint8_t *p, int depth) {')
        self.assertIn('VKA_ENV("DM_VK_UB_DEFER", 0)', ex)
        first = ex[ex.index('a render pass a draw opened stays open'):ex.index('case OP_END: return 0;')]
        self.assertIn('case OP_UPDATE_UB:', first)
        self.assertIn('vka_rp_ub_refuse(d, n)', first)
        self.assertIn('if (g_ub_np && h.op == OP_SIGNAL_EVENT)', ex)

    def test_shadow_never_drops_the_draw(self):
        f = body(self.draw, 'static int ubo_desc(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *bi) {')
        blk = f[f.index('if (vka_ub_pending_overlap(addr, size)) {'):f.index('VkBuffer b; VkDeviceSize o;')]
        self.assertNotIn('return 0', blk)
        self.assertIn('ub_shadow(addr, size, bi, NULL)', blk)
        sh = body(self.draw, 'static int ub_shadow(')
        ordered(self, sh, 'vka_tex_gw_shadow_read(addr, s.ptr, size)', 'vka_ub_overlay(addr, s.ptr, size)')

    def test_shadows_made_before_the_pass(self):
        # a shadow that cannot be made (no scratch, a write the shadow cannot read) ends the pass in ub_hazard, before rp_begin, instead of a stale bind in ubo_desc
        f = body(self.draw, 'static int ub_hazard(')
        self.assertIn('if (!ub_shadow(r->addr, us, &bi, &made)) return 1;', f)
        self.assertIn('if ((made_n += made) > UB_MEMO) return 1;', f)

    def test_cpu_reads_see_kept_updates(self):
        f = body(self.draw, 'static int cpu_read(uint64_t addr, void *out, size_t n) {')
        self.assertIn('vka_ub_cpu_overlay(addr, out, n);', f)
        self.assertIn('VKA_ENV("DM_VK_UB_CPU_OVERLAY", 1)', body(self.ex_src, 'void vka_ub_cpu_overlay('))

    def test_snapshot_not_borrowed(self):
        f = body(self.ex_src, 'static int ub_defer_try(')
        ordered(self, f, 'vka_scratch(n, 16, &s)', 'memcpy(s.ptr, data, (size_t)n);', 'g_ub_pending[g_ub_np++]')

    def test_no_game_gate(self):
        for name in ('vka_exec.c', 'vka_draw.c'):
            src = text(name)
            for head in ('static int ub_hazard(', 'int vka_rp_ub_refuse(', 'void vka_ub_flush_pass(', 'static int ub_defer_try(', 'static void ub_add('):
                if head in src:
                    self.assertNotIn('DM_GAME', body(src, head))
                    self.assertNotIn('OCARINA', body(src, head).upper())


if __name__ == '__main__':
    unittest.main()
