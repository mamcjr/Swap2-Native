"""A game with several registered builds (Mario Kart World 1.0.0 and 1.8.0: the same game, another module set and another RomFS) packs from a dump of either:
scripts/pack_android.py dump_modules takes the modules of the build row a dump matches, and optional_keys reads a profile's shader_optional.tsv, the shader keys only some builds have."""
import importlib.util
import tempfile
import unittest
from unittest import mock
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('pack_android', REPO / 'scripts/pack_android.py')
assert spec and spec.loader   # a path that exists: narrows the Optional for the type checker
pa = importlib.util.module_from_spec(spec); spec.loader.exec_module(pa)

A, B, C, D = ('%032x' % n for n in (1, 2, 3, 4))   # build IDs


class Profile:   # what dump_modules asks of a game profile
    dir = Path('.')
    id = ''   # (the title ID: split_shaders asks for it)
    def __init__(self, modules, builds): self._m, self._b = modules, builds
    def words(self, key): return self._m.split()
    def builds(self): return self._b


def nso(path, build_id):
    path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(build_id) + bytes(0x10))


class DumpModules(unittest.TestCase):
    G = Profile('main subsdk0 subsdk1 sdk', {'v0': {'display': '1.0.0', 'main': A, 'subsdk0': B, 'sdk': C},
                                             'v524288': {'display': '1.8.0', 'main': D, 'subsdk0': C, 'subsdk1': B, 'sdk': A}})

    def exefs(self, files):
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for n, i in files.items(): nso(d / n, i)
        return d

    def test_every_module_there(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': D, 'subsdk0': C, 'subsdk1': B, 'sdk': A})), ['main', 'subsdk0', 'subsdk1', 'sdk'])

    def test_build_with_one_module_less(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': A, 'subsdk0': B, 'sdk': C})), ['main', 'subsdk0', 'sdk'])

    def test_unregistered_set_is_left_to_the_caller(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': A, 'subsdk0': D, 'sdk': C})), ['main', 'subsdk0', 'subsdk1', 'sdk'])   # ids of no row: the caller says what is missing
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': A, 'sdk': C})), ['main', 'subsdk0', 'subsdk1', 'sdk'])


class OptionalKeys(unittest.TestCase):
    def test_file(self):
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (d / 'shader_optional.tsv').write_text('# comment\n00aa\t1.0.0\n11bb\t1.8.0\n')
        g = Profile('main', {}); g.dir = d
        self.assertEqual(pa.optional_keys(g), {'00aa', '11bb'})
        g.dir = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.assertEqual(pa.optional_keys(g), set())


class SplitAbsent(unittest.TestCase):
    """split_shaders: the keys of the profile's map that the dump lacks and shader_optional.tsv names go to <package>/_absent.tsv; any other missing key stops the pack"""

    def run_split(self, dump_keys, optional):
        root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        prof = root / 'profile'; prof.mkdir()
        row = lambda k: f'cs_x.bin\t0\tcompute\t{k}\tsrc\n'
        (prof / 'shader_map.tsv').write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(row(k) for k in ('k1', 'k2', 'k3')))
        if optional: (prof / 'shader_optional.tsv').write_text(''.join(f'{k}\t1.0.0\n' for k in optional))
        g = Profile('main', {}); g.dir = prof; g.id = '0000000000000000'
        def fake_run(cmd, env=None):   # shader_split.py: <romfs> <main> <pkg> <map> <facts> <pairs> <pso_pairs>
            Path(cmd[4]).mkdir(parents=True)
            Path(cmd[5]).write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(row(k) for k in dump_keys))
            for f in cmd[6:]: Path(f).write_text('')
            return type('R', (), {'returncode': 0})()
        args = type('A', (), {'shader_python': None})()
        pkg = root / 'pkg'
        with mock.patch.object(pa.subprocess, 'run', fake_run), mock.patch.object(pa, 'shader_python', lambda *a, **k: 'python'),                 mock.patch.object(pa, 'child_env', lambda **k: {}):
            pa.split_shaders(args, g, root / 'v', pkg, (root / 'romfs', root / 'exefs'))
        return pkg

    def test_optional_keys_are_listed(self):
        pkg = self.run_split(['k1', 'k2'], ['k3'])
        self.assertEqual((pkg / '_absent.tsv').read_text(), 'k3\n')

    def test_complete_dump_has_no_list(self):
        pkg = self.run_split(['k1', 'k2', 'k3'], ['k3'])
        self.assertFalse((pkg / '_absent.tsv').exists())

    def test_other_missing_key_stops(self):
        with self.assertRaises(pa.Error) as e: self.run_split(['k1'], ['k3'])
        self.assertIn('lack 1 of the 3', str(e.exception))


class SplitForwardsJobs(unittest.TestCase):
    """pack_android.py split_shaders forwards -J to shader_split.py as J (#482)"""

    def test_j_passed_to_shader_split(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            g = Profile('main', {}); g.id = '0100000000000000'; g.dir = root / 'prof'; g.dir.mkdir()
            seen = []

            def fake_run(cmd, env=None, **kw):
                seen.append(env)
                Path(cmd[4]).mkdir(parents=True, exist_ok=True)
                Path(cmd[5]).write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\na.bin\t0\tvs\tk1\tsrc\n')
                for f in cmd[6:]: Path(f).write_text('')
                return type('R', (), {'returncode': 0})()

            old = pa.subprocess.run, pa.shader_python
            pa.subprocess.run, pa.shader_python = fake_run, lambda *a, **k: 'python'   # pyright: ignore[reportAttributeAccessIssue]  # pa is loaded by path: a ModuleType
            try: pa.split_shaders(type('A', (), {'shader_python': None, 'J': 1})(), g, root / 'v', root / 'pkg', (root / 'romfs', root / 'exefs'))
            finally: pa.subprocess.run, pa.shader_python = old   # pyright: ignore[reportAttributeAccessIssue]
            self.assertEqual(seen[0].get('J'), '1')


if __name__ == '__main__':
    unittest.main()
