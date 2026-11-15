"""The texture pool slot memo of the Vulkan replay (vka_tex.c vka_tex_prepare, DM_VK_POOL_SLOT_DEDUP; docs/reference/rendering.md "Texture pool slots written again
unchanged on the Vulkan replay"). A model of the memo, keyed (texture ID, binding) -> (view handle, its g_vgen_h bucket), run against random view creation, destruction
(also inside write_tex, which lets the texture lock go) and handle recycling: a skipped write must never leave a slot naming another object than the one it would have
been written with, nor a dead one. Then the order the code must keep: g_vgen read before write_tex and again after the bucket, shown to fail when read once. Last, the source structure. No device, no game inputs (a game-neutral port of Erick's
test_ocarina_pool_descriptor_dedup.py)."""
from pathlib import Path
import random
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / 'dm/gfx/vk_android/vka_tex.c'
NULL = 0


class Device:
    """view handles as the driver hands them out: a destroyed handle may come back for a new view; objects are told apart by a unique id. vgen: g_vgen; hgen:
    g_vgen_h, one counter per bucket of handles (few buckets here, so unrelated handles share them as in the C table)"""
    def __init__(self, rng, buckets=4):
        self.rng, self.free, self.next, self.live, self.uid = rng, [], 1, {}, 0
        self.vgen, self.hgen, self.nb = 0, [0] * buckets, buckets
        self.obj = {}       # handle -> the object id it names, also once destroyed (a slot written with it keeps naming that dead object)

    def create(self, recycle=None):
        if recycle is None:
            recycle = self.rng.random() < 0.7
        h = self.free.pop(self.rng.randrange(len(self.free))) if self.free and recycle else self._new()
        self.uid += 1
        self.live[h] = self.uid; self.obj[h] = self.uid
        return h

    def _new(self):
        h = self.next; self.next += 1; return h

    def bucket(self, h):
        return self.hgen[h % self.nb]

    def destroy(self, h):
        self.vgen += 1          # img_destroy: g_vgen first, then the handle's bucket, then vkDestroyImageView
        self.hgen[h % self.nb] += 1
        del self.live[h]
        self.free.append(h)


class Memo:
    """mode 'code': vka_tex_prepare's rule (g0 before write_tex; the bucket b, then g1, after; skip or store only when g0 == g1, keyed on b). 'after': g_vgen read once after
    write_tex and stored (the order before the 2026-11-13 review). 'before': g_vgen read once before write_tex"""
    def __init__(self, mode='code'):
        self.m, self.mode = {}, mode

    def write(self, slot, view, g0, b, g1, dedup=True):
        """1 = the descriptor write is sent"""
        if self.mode == 'code':
            if g0 != g1:
                self.m.pop(slot, None); return 1
            key = (view, b)
        else:
            key = (view, g1 if self.mode == 'after' else g0)
        if dedup and self.m.get(slot) == key:
            return 0
        self.m[slot] = key
        return 1


def run(seed, steps, mode):
    """the descriptor set's real content (by object id) against what the memo lets through"""
    rng = random.Random(seed)
    dev, memo, held = Device(rng), Memo(mode), {}
    want = {}               # slot -> the view write_tex picks now
    skips = 0
    for _ in range(steps):
        r = rng.random()
        if r < 0.25 or not dev.live:
            slot = (rng.randrange(8), rng.randrange(2))
            want[slot] = dev.create()
        elif r < 0.45:
            h = rng.choice(list(dev.live))
            if h not in want.values():          # only views no slot is about to be written with go (vka_after: the GPU is done with them)
                dev.destroy(h)
        elif r < 0.55:
            want[(rng.randrange(8), rng.randrange(2))] = NULL
        else:
            if not want:
                continue
            slot = rng.choice(list(want))
            view = want[slot]
            if view != NULL and view not in dev.live:
                continue                        # (its image went: the registry would name another view)
            g0 = dev.vgen
            x = rng.random()
            if x < 0.15 and view:               # write_tex read the view, then its image was retired and destroyed while g_tmu was let go
                dev.destroy(view); del want[slot]
            elif x < 0.3 and view:              # a destroy and a recycle inside write_tex: it hands out the recycled handle for a new view
                dev.destroy(view); view = dev.create(recycle=True); want[slot] = view
            elif x < 0.45 and dev.live:         # an unrelated destroy inside write_tex
                v = rng.choice(list(dev.live))
                if v not in want.values():
                    dev.destroy(v)
            b = dev.bucket(view) if view else 0
            g1 = dev.vgen
            obj = dev.obj.get(view, 0) if view else 0
            if memo.write(slot, view, g0, b, g1):
                held[slot] = obj
            else:
                skips += 1
                if held.get(slot) != obj or (view and view not in dev.live):
                    return False, skips
    return True, skips


class Model(unittest.TestCase):
    def test_random_stress(self):
        total = 0
        for seed in range(6):
            ok, skips = run(seed, 30000, 'code')
            self.assertTrue(ok, f'seed {seed}: a skipped write left a slot naming another object')
            total += skips
        self.assertGreater(total, 0, 'the memo never skipped: the model does not exercise it')

    def test_gen_read_once_fails(self):
        self.assertFalse(all(run(seed, 30000, 'before')[0] for seed in range(20)), 'g_vgen read once before write_tex should be shown unsafe')
        # (read once after write_tex fails only when no other destroy comes between the dead view's store and its handle's recycle, which the random
        # walk rarely reaches: test_destroy_inside_write_tex_then_recycle builds it)

    def test_destroy_inside_write_tex_then_recycle(self):
        """the review's case: write_tex reads V, V is destroyed before the generation is read, and V's handle comes back later for another view"""
        for mode, want_sent in (('after', 0), ('code', 1)):
            dev, memo = Device(random.Random(0)), Memo(mode)
            v = dev.create()
            g0 = dev.vgen; dev.destroy(v)
            memo.write((1, 0), v, g0, dev.bucket(v), dev.vgen)
            v2 = dev.create(recycle=True); self.assertEqual(v2, v)
            g0 = dev.vgen
            sent = memo.write((1, 0), v2, g0, dev.bucket(v2), dev.vgen)
            self.assertEqual(sent, want_sent, f'{mode}: the recycled handle must be written by the code, and was skipped by the old order')

    def test_unrelated_destroy_keeps_memo(self):
        dev, memo = Device(random.Random(0), buckets=4096), Memo()
        v, other = dev.create(recycle=False), dev.create(recycle=False)
        self.assertEqual(memo.write((1, 0), v, dev.vgen, dev.bucket(v), dev.vgen), 1)
        dev.destroy(other)
        self.assertEqual(memo.write((1, 0), v, dev.vgen, dev.bucket(v), dev.vgen), 0, "another view's destroy leaves this slot's memo warm")

    def test_bindings_apart_and_null(self):
        m = Memo()
        self.assertEqual(m.write((5, 0), 7, 0, 0, 0), 1)
        self.assertEqual(m.write((5, 1), 7, 0, 0, 0), 1, 'the storage slot of an ID is not its sampled slot')
        self.assertEqual(m.write((5, 0), 7, 0, 0, 0), 0)
        self.assertEqual(m.write((5, 0), NULL, 0, 0, 0), 1, 'a view to null is a write')
        self.assertEqual(m.write((5, 0), NULL, 0, 0, 0), 0, 'the null view again is skipped')
        self.assertEqual(m.write((5, 0), 7, 0, 0, 0), 1, 'null to a view is a write')
        self.assertEqual(m.write((5, 0), 7, 0, 1, 0), 1, 'a view of its bucket destroyed since: written')
        self.assertEqual(m.write((5, 0), 7, 1, 1, 2), 1, 'a destroy inside write_tex: written')
        self.assertEqual(m.write((5, 0), 7, 2, 1, 2), 1, '...and its memo dropped')
        self.assertEqual(m.write((5, 0), 7, 2, 1, 2, dedup=False), 1, 'switch off: written')


def body(text, head):
    """the code of the function that starts at head, to its closing brace at column 0, without its // comments"""
    i = text.index(head)
    return re.sub(r'//[^\n]*', '', text[i:text.index('\n}\n', i)])


class Source(unittest.TestCase):
    def setUp(self):
        self.t = SRC.read_text(errors='replace')

    def test_vgen_bump_first_in_img_destroy(self):
        f = body(self.t, 'static void img_destroy(void *arg) {')
        code = [l for l in f.splitlines()[1:] if l.strip()]
        self.assertIn('atomic_fetch_add_explicit(&g_vgen, 1', code[0], "the g_vgen bump is img_destroy's first statement")
        self.assertLess(f.index('&g_vgen'), f.index('vkDestroyImageView'))
        self.assertLess(f.index('&g_vgen'), f.index('img_destroy(im->zold)'))
        self.assertLess(f.index('atomic_fetch_add_explicit(vgen_h(v->v), 1'), f.index('vkDestroyImageView'), "the handle's bucket bumped before its destroy")

    def test_prepare_loop(self):
        f = body(self.t, 'void vka_tex_prepare(VkCommandBuffer cb) {')
        i = f.index('write_tex(id, cb, &w[nw], &ii[nw]);')
        loop = f[i:f.index('g_ntd = 0;', i)]
        pre = f[f.rindex('\n', 0, f.rindex('\n', 0, i)):i]
        self.assertIn('uint32_t g0 = atomic_load_explicit(&g_vgen, memory_order_acquire);', pre, 'g0 read just before write_tex')
        self.assertIn('VKA_ENV("DM_VK_POOL_SLOT_DEDUP", 1)', loop)
        self.assertLess(loop.index('slot_write_keep'), loop.index('vgen_h(view)'), 'the null-view rule first')
        self.assertLess(loop.index('vgen_h(view)'), loop.index('g1 = atomic_load_explicit(&g_vgen'), 'the bucket read before g1')
        self.assertLess(loop.index('g1 = atomic_load_explicit(&g_vgen'), loop.index('g_pool_written['), 'the memo pointer read after write_tex')
        self.assertIn('if (g0 != g1) { if (p) p->valid &= (uint8_t)~(1u << kind); }', loop, 'a destroy inside write_tex: written, memo dropped')
        self.assertNotIn('g_tgen', loop, 'keyed on the handle buckets, not g_tgen')
        self.assertNotIn('DM_GAME', loop)
        # the memo is stored with the switch off too
        self.assertRegex(loop, r'else if \(p\) \{ p->view\[kind\] = view; p->gen\[kind\] = b;')

    def test_memo_grows_with_registry(self):
        f = body(self.t, 'static void treg(int id, const tex_t *t, const view_t *v, int img) {')
        self.assertLess(f.index('g_treg = realloc'), f.index('g_pool_written, (size_t)g_ntreg'))
        self.assertIn('g_pool_written = NULL; g_pool_written_cap = 0;', f)

    def test_only_writer(self):
        n = len(re.findall(r'vk\.vkDestroyImageView\(', self.t))
        self.assertEqual(n, 1, 'img_destroy is the only place vka_tex.c destroys views')

    def test_live_switch(self):
        ctl = (ROOT / 'dm/gfx/vk_android/vka_ctl.c').read_text(errors='replace')
        live = ctl[ctl.index('g_live[]'):]
        self.assertIn('"DM_VK_POOL_SLOT_DEDUP"', live[:live.index('NULL}')])


if __name__ == '__main__':
    unittest.main()
