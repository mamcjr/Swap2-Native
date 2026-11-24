"""Execute Ocarina of Time's screen-shape stub for the movie's viewport on ARM64, without game files: the title screen's background and the opening are a 16:9
movie drawn into the whole frame, so on a taller screen the stub keeps its viewport 16:9 about the centre, and leaves it alone at 16:9 or with the shape off.
In this fork the hooks are dormant: patch_nso.py runs the build's FRAME_STEP without the located functions (ctx.others) upstream's MATCH_FRAME_STEP hands the stub, so the
rewritten main keeps only the frame step (patches.py, Screen shape). The placement and execution tests hand screen_shape the functions as upstream's matcher would.

Optional dependency: unicorn. On macOS its JIT needs execution outside the sandbox.
"""
import importlib
import importlib.util
from pathlib import Path
import struct
import sys
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    uc = importlib.import_module('unicorn')
    arm = importlib.import_module('unicorn.arm64_const')
except ImportError:
    uc = arm = None

spec = importlib.util.spec_from_file_location('ocarina_patches', Path(__file__).resolve().parents[2] / 'games/0400486024DEC000/patches.py')
assert spec is not None and spec.loader is not None
patch = importlib.util.module_from_spec(spec); spec.loader.exec_module(patch)
OTHERS = patch.screen_shape_functions
FUNCTIONS = {name: 0x100000 + i * 0x10000 for i, name in enumerate(OTHERS)}


def record_hooks(others, run=None):
    sites, capabilities, cursor = {}, set(), [0x8000000]

    def emit(_word):
        cursor[0] += 4

    ctx = types.SimpleNamespace(here=lambda: cursor[0], emit=emit, site=sites.__setitem__, put=lambda off, words, why: None,
                                state=0x9000000, base=0, others=others, capabilities=capabilities)
    (run or patch.screen_shape)(ctx)
    return sites, capabilities


class Placement(unittest.TestCase):
    def test_movie_draw_site(self):
        # the movie's draw is one of the screen shape's functions, and its stub replaces the instruction after the viewport is built (ldp x9, x8, [x19, #0x18] at +0x64)
        self.assertEqual(OTHERS['movie draw']['size'], 0x158)
        sites, capabilities = record_hooks(FUNCTIONS)
        self.assertEqual(sites[FUNCTIONS['movie draw'] + 0x64], '69a241a9')
        self.assertEqual(len(sites), 19)   # the 19 aspect hooks
        self.assertEqual(capabilities, {'SCREEN_ASPECT'})

    def test_any_missing_aspect_match_leaves_all_aspect_hooks_out(self):
        for name in patch.screen_shape_functions:
            with self.subTest(name=name):
                sites, capabilities = record_hooks(FUNCTIONS | {name: None})
                self.assertEqual(sites, {})
                self.assertEqual(capabilities, set())

    def test_frame_step_with_located_functions_adds_the_hooks(self):
        sites, capabilities = record_hooks(FUNCTIONS, patch.frame_step)
        self.assertEqual(set(sites) - {0x6e6cc4}, set(record_hooks(FUNCTIONS)[0]))
        self.assertEqual(capabilities, {'SCREEN_ASPECT'})

    def test_this_forks_rewriter_installs_only_the_frame_step(self):
        # patch_nso.py's FRAME_STEP context (no others, no capabilities): the v131072 main is rewritten as before the port, so a phone's picture cannot change
        sites, cursor = {}, [0x8000000]
        ctx = types.SimpleNamespace(here=lambda: cursor[0], emit=lambda w: cursor.__setitem__(0, cursor[0] + 4), site=sites.__setitem__,
                                    put=lambda off, words, why: None, state=0x9000000, render_scale=0)
        patch.frame_step(ctx)
        self.assertEqual(sites, {0x6e6cc4: '2818201e'})


class Arm64ScreenShape(unittest.TestCase):
    base, area, state, frame = 0x4000000, 0x2000000, 0x9000000, 0xa000000
    movie = 0x3b5c710

    def setUp(self):
        assert uc is not None and arm is not None
        self.arm = arm
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x4000000), (self.state, 0x1000), (self.frame, 0x1000)):
            self.u.mem_map(a, n)
        self.cursor = self.area
        ctx = types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None, put=self.put, state=self.state - self.base,
                                    others=FUNCTIONS | {'movie draw': self.movie}, capabilities=set())
        patch.screen_shape(ctx)

    def emit(self, w):
        self.u.mem_write(self.base + self.cursor, struct.pack('<I', w)); self.cursor += 4

    def put(self, off, words, why):
        self.u.mem_write(self.base + off, struct.pack('<' + 'I' * len(words), *words))


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class MovieViewport(Arm64ScreenShape):
    def run_viewport(self, oh, rect):
        self.u.mem_write(self.state + 0x48, struct.pack('<I', oh))
        self.u.mem_write(self.frame + 0x100 - 0x28 + 8, struct.pack('<4f', *rect))
        self.u.reg_write(self.arm.UC_ARM64_REG_X29, self.frame + 0x100)
        self.u.reg_write(self.arm.UC_ARM64_REG_X19, self.frame + 0x800)
        end = self.base + self.movie + 0x68
        self.u.emu_start(end - 4, end, count=100)
        self.assertEqual(self.u.reg_read(self.arm.UC_ARM64_REG_PC), end)
        return struct.unpack('<4f', self.u.mem_read(self.frame + 0x100 - 0x28 + 8, 16))

    def test_taller_screens_keep_the_movie_16_9(self):
        self.assertEqual(self.run_viewport(1440, (0, 0, 2560, 1920)), (0, 240, 2560, 1680))   # 4:3: bars of 240 above and below
        self.assertEqual(self.run_viewport(1200, (0, 0, 2560, 1600)), (0, 80, 2560, 1520))    # 16:10
        self.assertEqual(self.run_viewport(1280, (0, 0, 1920, 1280)), (0, 100, 1920, 1180))   # 3:2 at 1920 wide

    def test_16_9_and_shape_off_stay(self):
        self.assertEqual(self.run_viewport(1440, (0, 0, 2560, 1440)), (0, 0, 2560, 1440))     # a 16:9 frame
        self.assertEqual(self.run_viewport(0, (0, 0, 2560, 1920)), (0, 0, 2560, 1920))        # shape off (oh 0): the game's own


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class PaneGuard(Arm64ScreenShape):
    def run_load_window(self, *, name=b'N_SlideList_00', kind=7, authored_height=1984.0, count=7, active=True):
        # A retail-shaped synthetic pane array with the Load list window at index 5.
        layout, res, nodes = self.frame, self.frame + 0x180, self.frame + 0x400
        records, names, record = self.frame + 0x200, self.frame + 0x240, self.frame + 0x980
        pane = nodes + 5 * 0xb0
        title, node_name = self.frame + 0x900, self.frame + 0x930
        for addr, fmt, value in (
            (self.state + 0x48, 'I', 1440 if active else 0), (self.state + 0x5c, 'f', 4 / 3),
            (pane + 0x18, 'Q', layout), (pane + 0xa8, 'I', kind << 22), (pane + 0x5c, 'f', 1984.0),
            (layout + 0x10, 'Q', res), (layout + 0x100, 'Q', nodes), (layout + 0x13c, 'H', count),
            (res, 'I', 0x004c4955), (res + 0x18, 'Q', title), (res + 0x20, 'Q', names), (res + 0x28, 'Q', records), (res + 0x38, 'I', 7),
            (records + 5 * 8, 'Q', record), (names + 5 * 8, 'Q', node_name),
            (record + 0x38, 'I', 7 << 7), (record + 0x30, 'f', authored_height),
        ):
            self.u.mem_write(addr, struct.pack('<' + fmt, value))
        self.u.mem_write(title, b'SystemLoadList_00\0')
        self.u.mem_write(node_name, name + b'\0')
        self.u.reg_write(self.arm.UC_ARM64_REG_X19, pane)
        self.u.reg_write(self.arm.UC_ARM64_REG_SP, self.frame + 0xf00 if active else 0)
        site = self.base + FUNCTIONS['pane global matrix'] + 0x3b0
        self.u.emu_start(site, site + 4, count=1000)
        self.assertEqual(self.u.reg_read(self.arm.UC_ARM64_REG_PC), site + 4)
        return struct.unpack('<f', self.u.mem_read(pane + 0x5c, 4))[0]

    def test_only_the_reviewed_pane_can_change_height(self):
        self.assertEqual(self.run_load_window(name=b'N_Other_00'), 1984.0)
        self.assertEqual(self.run_load_window(kind=1), 1984.0)
        self.assertEqual(self.run_load_window(authored_height=1700.0), 1984.0)
        self.assertEqual(self.run_load_window(count=5), 1984.0)
        self.assertAlmostEqual(self.run_load_window(), 2344.0, places=3)

    def test_shape_off_replays_without_stack_or_call(self):
        self.assertEqual(self.run_load_window(active=False), 1984.0)


if __name__ == '__main__':
    unittest.main()
