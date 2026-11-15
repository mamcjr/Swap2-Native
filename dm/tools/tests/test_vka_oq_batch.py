"""Render conditions' index and occlusion queries read by list on the Vulkan replay (vka_exec.c "Render conditions", "Occlusion queries"; docs/reference/rendering.md
"Render-condition lookups and occlusion readbacks by list on the Vulkan replay"), checked in the source: the index put on append and rebuilt after a compaction that
removed entries, rc_known through it; the list's queries read and given back under g_oq_mu around the whole post_run loop, behind DM_VK_OQ_BATCH, with the
report-by-report path kept and no max() on the ring. The pure rules are checked by dm/host/tests/t_android_rcix.c. No device, no game inputs."""
from pathlib import Path
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


class Index(unittest.TestCase):
    def setUp(self):
        self.src = text('vka_exec.c')

    def test_kept_in_step_with_the_list(self):
        self.assertIn('rcix_put(&g_rcix, a, (uint32_t)(g_nrcp - 1));', body(self.src, 'static void rc_note(uint64_t a, int kind) {'))
        self.assertIn('if (j != n0) rcix_rebuild(&g_rcix, g_rcp, sizeof *g_rcp, g_nrcp);', body(self.src, 'static void rc_prune(void) {'))
        self.assertIn('_Static_assert(RCIX_CAP >= 2 * RC_PEND_MAX', self.src)
        self.assertEqual(self.src.count('g_nrcp = '), 1, 'g_nrcp changes only in rc_prune (and grows in rc_note): every change keeps the index')

    def test_lookup(self):
        f = body(self.src, 'static int rc_known(uint64_t a, uint64_t *v, int *why) {')
        self.assertIn('rcix_find(&g_rcix, a)', f)
        self.assertNotIn('for (int i = g_nrcp - 1', f, 'the reverse scan is gone')


class Batch(unittest.TestCase):
    def setUp(self):
        self.src = text('vka_exec.c')

    def test_switch_and_sizes(self):
        self.assertIn('VKA_ENV("DM_VK_OQ_BATCH", 1)', body(self.src, 'static int oq_on(void) {'))
        self.assertIn('OQ_BATCH = 1024', self.src)
        self.assertIn('if (p->found && q0 != p->last) { p->bad = 1; return; }', text('vka_oqbatch.h'))

    def test_report_by_report_kept(self):
        f = body(self.src, 'static void oq_done(const pop *o, uint64_t ticks, const oq_batch *b) {')
        self.assertIn('if (k > OQ_RB) k = OQ_RB;', f)
        self.assertIn('if (!b->on && o->q1 > o->q0) {', f)
        self.assertNotIn('pthread_mutex_lock', f, 'post_run holds the lock')

    def test_lock_over_the_list(self):
        f = body(self.src, 'static void post_run(void *ops_, int n) {')
        ordered(self, f, 'pthread_mutex_lock(&g_oq_mu); oq_batch_init(&oqb, ops, n);', 'oq_done(&ops[i], ticks, &oqb)',
                'oq_batch_release(&oqb); pthread_mutex_unlock(&g_oq_mu);', 'free(ops);')

    def test_no_max_on_the_ring(self):
        for name in ('oq_batch_release', 'oq_done'):
            f = body(self.src, 'static void %s(' % name)
            self.assertNotIn('max(', f)
        self.assertIn('atomic_store_explicit(&g_oq_ready, b->last + OQ_N, memory_order_release);', body(self.src, 'static void oq_batch_release('))

    def test_no_retry_on_a_lost_device(self):
        # A hang runs every pending post_run: a per-query retry there would be up to OQ_N failing driver calls a list under g_oq_mu
        f = body(self.src, 'static void oq_batch_init(')
        self.assertIn('if (e < 0 && e != VK_ERROR_DEVICE_LOST && !vka_hung()) for (uint32_t j = 0; j < k; j++) {', f)
        self.assertIn('if (e1 == VK_ERROR_DEVICE_LOST) break;', f)


if __name__ == '__main__':
    unittest.main()
