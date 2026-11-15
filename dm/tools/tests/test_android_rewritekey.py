"""The code key of scripts/pack_android.py (<module>.android.key: rewrite_key, rewrite_keys, upgrade_keys, build_rows) and the launcher's RewriteKey.java (driven by
android/app/src/jvmtest's RewriteKeyJvmTest: the same keys, so Home shows no "other tools" chip), on synthetic trees only.
The key hashes game.tsv's build rows only (the part patch_nso.py reads): a change to another row (a host key such as ndsr_input) keeps it, a build row change does
not. Another profile's patches.py is in the key when it names the module's build ID (patch_nso.py loads every patches.py). A key of the earlier formula (the whole
game.tsv) counts with the profile's game.tsv or with the pack's copy when that copy's build rows are the profile's; a differing build row, rewriter file, module or
patches.py is refused; rewrite keeps such a module and writes its key in the current formula, and --keep-code's upgrade_keys does the same (keeping a ' mods='
part). The JVM part is skipped without a JDK."""
import hashlib
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'scripts'))
import pack_android  # noqa: E402

SRC = REPO / 'android/app/src/main/java/dev/nxnative/android/RewriteKey.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/RewriteKeyJvmTest.java'
GAME = '0100000000000000'
OTHER = '0100000000001000'
BID = bytes(range(0x10, 0x30))   # the module's build ID (NSO header +0x40)
PROFILE = '# a build row comment\nname\tTest\nbuild\tv1\t1.0.0\tmain=aa\tsdk=bb\nmodules\tmain sdk\nndsr_input\t720\n'
HAVE_JDK = bool(shutil.which('javac') and shutil.which('java'))


def nso():
    return b'NSO0' + bytes(0x3c) + BID + bytes(range(256)) * 40


def part(h, name, b): h.update(name.encode() + b'\0' + b)


class Tree(unittest.TestCase):   # a tree of rewriter files, two profiles, a module
    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='rwkey_'))
        self.tree = self.t / 'tree'
        tools = self.tree / 'dm/tools'; tools.mkdir(parents=True)
        for f in pack_android.REWRITER + ('exefs_patches.py',): (tools / f).write_bytes(b'current ' + f.encode())
        self.gdir = self.tree / 'dm/games' / GAME; self.gdir.mkdir(parents=True)
        (self.gdir / 'game.tsv').write_text(PROFILE, newline=''); (self.gdir / 'patches.py').write_text('# patches\n')
        self.odir = self.tree / 'dm/games' / OTHER; self.odir.mkdir(parents=True)
        (self.odir / 'game.tsv').write_text('name\tOther\n'); (self.odir / 'patches.py').write_text('# other patches\n')
        self.mod = self.t / 'main'; self.mod.write_bytes(nso())
        self.g = types.SimpleNamespace(dir=self.gdir, id=GAME)
        self.pack = self.t / 'pack'; self.pack.mkdir()
        self.saved = pack_android.REPO
        pack_android.REPO = self.tree

    def tearDown(self):
        pack_android.REPO = self.saved
        shutil.rmtree(self.t, ignore_errors=True)

    def v1(self, gt, rewriter=None, mod=None, exefs=False):   # the earlier formula's key: the whole game.tsv
        h = hashlib.sha256()
        for f in pack_android.REWRITER: part(h, f, (rewriter or {}).get(f, b'current ' + f.encode()))
        if exefs: part(h, 'exefs_patches.py', b'current exefs_patches.py')
        part(h, 'main', mod or self.mod.read_bytes()); part(h, 'patches.py', (self.gdir / 'patches.py').read_bytes()); part(h, 'game.tsv', gt.encode())
        return h.hexdigest() + ' target=android'

    def py_keys(self, mods=(), pack=None): return pack_android.rewrite_keys(self.mod, self.g, mods, pack)

    def java_keys(self, pack_game=None, exefs=False, suffix=None):
        classes = self.t / 'classes'
        if not classes.is_dir():
            r = subprocess.run(['javac', '-Xlint:all', '-Werror', '-d', str(classes), str(SRC), str(TEST)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        props = [f'-Dpack.game={pack_game}'] * bool(pack_game) + ['-Dexefs=1'] * exefs + [f'-Dsuffix={suffix}'] * bool(suffix)
        r = subprocess.run(['java', *props, '-cp', str(classes), 'dev.nxnative.android.RewriteKeyJvmTest', str(self.tree), str(self.mod), f'dm/games/{GAME}/', 'main',
                            BID[:0x10].hex(), *pack_android.REWRITER], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.split('\n')[:-1]

    def accepted(self, key, pack_gt):   # (Python, Java) verdicts on a key with the pack's game.tsv pack_gt (None: the pack has none)
        (self.pack / 'game.tsv').unlink(missing_ok=True)
        if pack_gt is not None: (self.pack / 'game.tsv').write_text(pack_gt, newline='')
        py = key in self.py_keys(pack=self.pack)
        if not HAVE_JDK: return py, py
        return py, key in self.java_keys(self.pack / 'game.tsv' if pack_gt is not None else None)


class Rows(Tree):
    def rows_java(self, b):
        if not HAVE_JDK: return pack_android.build_rows(b)
        self.java_keys()   # compiles
        f = self.t / 'rows.tsv'; f.write_bytes(b)
        r = subprocess.run(['java', '-cp', str(self.t / 'classes'), 'dev.nxnative.android.RewriteKeyJvmTest', '--rows', str(f)], capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_build_rows(self):
        cases = [
            (b'# c build\nbuild \t v1\t1.0.0 \tmain=aa\r\nname\tX\nbuilds\tno\n', b'build\tv1\t1.0.0\tmain=aa\n'),
            (PROFILE.encode(), b'build\tv1\t1.0.0\tmain=aa\tsdk=bb\n'),
            (b'x\tbuild\x0bbuild\tv2\n', b'x\tbuild\x0bbuild\tv2\n'),   # splitlines would split there: the line is kept whole
            (b'build\xc2\xa0\tv3\n# Pok\xc3\xa9mon build\n# Pok\xc3\xa9mon\n', b'build\xc2\xa0\tv3\n# Pok\xc3\xa9mon build\n'),   # non-ASCII with build: whole
            (b'build\tv4\r\r\n', b'build\tv4\r\n'),   # a second \r is no line end here, and patch_nso.py splits at it
        ]
        for b, want in cases:
            self.assertEqual(pack_android.build_rows(b), want, b)
            self.assertEqual(self.rows_java(b), want, b)
        self.assertEqual(pack_android.build_rows(PROFILE.encode()), pack_android.build_rows(PROFILE.replace('720', '1080').encode() + b'scene_scale_palette\t0.6\n'))


class Formula(Tree):
    def test_current_key_ignores_other_rows(self):
        k = pack_android.rewrite_key(self.mod, self.g)
        (self.gdir / 'game.tsv').write_text(PROFILE.replace('720', '1080') + 'scene_scale_palette\t0.6135\n# a comment\n')
        self.assertEqual(pack_android.rewrite_key(self.mod, self.g), k)
        (self.gdir / 'game.tsv').write_text(PROFILE.replace('main=aa', 'main=ac'))
        self.assertNotEqual(pack_android.rewrite_key(self.mod, self.g), k)

    def test_inputs_in_the_key(self):
        k = pack_android.rewrite_key(self.mod, self.g)
        for f, b in (('dm/tools/patch_nso.py', b'new'), ('dm/tools/sdk_patches.py', b'new'), (f'dm/games/{GAME}/patches.py', b'# new\n')):
            p = self.tree / f; old = p.read_bytes(); p.write_bytes(b)
            self.assertNotEqual(pack_android.rewrite_key(self.mod, self.g), k, f)
            p.write_bytes(old)
        self.assertEqual(pack_android.rewrite_key(self.mod, self.g), k)

    def test_other_profile_patches(self):   # another game's patches.py: in the key only when it names the module's build
        k = pack_android.rewrite_key(self.mod, self.g)
        (self.odir / 'patches.py').write_text('# other patches, changed\n')
        self.assertEqual(pack_android.rewrite_key(self.mod, self.g), k)
        (self.odir / 'patches.py').write_text(f"CUTS = {{'{BID[:0x10].hex()}': []}}\n")
        k2 = pack_android.rewrite_key(self.mod, self.g)
        self.assertNotEqual(k2, k)
        self.assertEqual(len(self.py_keys()), 1)   # no earlier formula: it never hashed that file
        (self.odir / 'patches.py').write_text(f"CUTS = {{'{BID[:0x10].hex()}': [(0, '00', '01', 'x')]}}\n")
        self.assertNotEqual(pack_android.rewrite_key(self.mod, self.g), k2)
        if HAVE_JDK: self.assertEqual(self.java_keys(), self.py_keys())

    @unittest.skipUnless(HAVE_JDK, 'no JDK')
    def test_java_same_keys(self):
        (self.pack / 'game.tsv').write_text(PROFILE.replace('720', '1080'), newline='')
        self.assertEqual(self.java_keys(self.pack / 'game.tsv'), self.py_keys(pack=self.pack))
        self.assertEqual(len(self.py_keys(pack=self.pack)), 3)
        files = [('m/exefs/a.ips', self.mod)]
        py = self.py_keys(files)
        self.assertTrue(py[0].endswith(' mods=' + pack_android.exefs_patches.digest(files)))
        suffix = py[0][py[0].index(' '):]
        self.assertEqual(self.java_keys(exefs=True, suffix=suffix), py)

    def test_old_key_other_rows_differ(self):   # the profile gained host rows since the pack was made: accepted through the pack's copy
        old = PROFILE.replace('ndsr_input\t720\n', '')
        self.assertEqual(self.accepted(self.v1(old), old), (True, True))
        self.assertEqual(self.accepted(self.v1(old), None), (False, False))   # no copy: refused
        self.assertEqual(self.accepted(self.v1(old), PROFILE.replace('720', '1080')), (False, False))   # a copy the key was not made with

    def test_old_key_profile_unchanged(self):
        self.assertEqual(self.accepted(self.v1(PROFILE), None), (True, True))

    def test_old_key_build_row_differs(self):
        old = PROFILE.replace('main=aa', 'main=ac')
        self.assertEqual(self.accepted(self.v1(old), old), (False, False))

    def test_old_key_other_inputs_differ(self):
        old = PROFILE.replace('720', '1080')
        self.assertEqual(self.accepted(self.v1(old, rewriter={'patch_nso.py': b'older'}), old), (False, False))
        self.assertEqual(self.accepted(self.v1(old, mod=b'another module'), old), (False, False))

    def test_rewrite_keeps_and_upgrades(self):
        old = PROFILE.replace('720', '1080')
        (self.pack / 'main.patched.bin').write_bytes(b'code'); (self.pack / 'game.tsv').write_text(old, newline='')
        (self.pack / 'main.android.key').write_text(self.v1(old) + '\n')
        pack_android.upgrade_keys({'main': self.mod}, self.pack, self.g)
        self.assertEqual((self.pack / 'main.android.key').read_text().strip(), pack_android.rewrite_key(self.mod, self.g))
        (self.pack / 'main.android.key').write_text(self.v1(old) + '\n')
        pack_android.rewrite(sys.executable, {'main': self.mod}, self.pack, 1, self.g)
        self.assertEqual((self.pack / 'main.patched.bin').read_bytes(), b'code')   # kept: no rewrite ran
        self.assertEqual((self.pack / 'main.android.key').read_text().strip(), pack_android.rewrite_key(self.mod, self.g))

    def test_upgrade_keeps_mods_part(self):
        old = PROFILE.replace('720', '1080')
        (self.pack / 'game.tsv').write_text(old, newline='')
        key = self.v1(old, exefs=True) + ' mods=abc'
        (self.pack / 'main.android.key').write_text(key + '\n')
        pack_android.upgrade_keys({'main': self.mod}, self.pack, self.g)
        got = (self.pack / 'main.android.key').read_text().strip()
        self.assertTrue(got.endswith(' target=android mods=abc'))
        self.assertEqual(got.split(' ')[0], pack_android.rewrite_keys(self.mod, self.g, xp=True)[0].split(' ')[0])

    def test_upgrade_leaves_other_keys(self):
        (self.pack / 'main.android.key').write_text('other target=android\n')
        pack_android.upgrade_keys({'main': self.mod}, self.pack, self.g)
        self.assertEqual((self.pack / 'main.android.key').read_text(), 'other target=android\n')


if __name__ == '__main__':
    unittest.main()
