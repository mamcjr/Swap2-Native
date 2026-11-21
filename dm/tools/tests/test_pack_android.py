"""scripts/pack_android.py's swap of a rewritten module into a pack (the phone's packer updates the user's pack in place): the module's files go in from its
staging folder by rename and its key file comes last, so a pack stopped before the key is written has a module without a key, which the launcher refuses."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'scripts'))
import pack_android   # noqa: E402


class Swap(unittest.TestCase):
    def test_swap_in_replaces_files_and_writes_the_key_last(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            (out / 'main.patched.bin').write_bytes(b'old'); (out / 'main.fixups.tsv').write_text('old\n')
            st = pack_android.staging(out, 'main'); st.mkdir()
            (st / 'main.patched.bin').write_bytes(b'new'); (st / 'main.fixups.tsv').write_text('new\n'); (st / 'build_src.sha256').write_text('x\n')
            seen = []
            real = os.replace
            def spy(a, b): seen.append(Path(b).name); real(a, b)
            pack_android.os.replace = spy
            try: pack_android.swap_in(out, 'main', 'k target=android')
            finally: pack_android.os.replace = real
            self.assertEqual(seen[-1], 'main.android.key', 'the key must go in last')
            self.assertEqual((out / 'main.patched.bin').read_bytes(), b'new')
            self.assertEqual((out / 'main.fixups.tsv').read_text(), 'new\n')
            self.assertEqual((out / 'main.android.key').read_text(), 'k target=android\n')
            self.assertFalse(st.exists())


class CopyOnExfat(unittest.TestCase):
    """exFAT storage under Android's FUSE (/mnt/media_rw) refuses chmod and utime, so shutil.copystat raised PermissionError at the end of Prepare
    (Banana Native issue #360, test from eden/v0.3.43 af1ed596, MIT)"""

    def setUp(self):
        import shutil
        self.d = tempfile.TemporaryDirectory(); self.addCleanup(self.d.cleanup)
        self.src = Path(self.d.name) / 'src.bin'; self.src.write_bytes(os.urandom(300_000))
        self.dst = Path(self.d.name) / 'dst.bin.partial'
        def deny(*a, **k): raise PermissionError(1, 'Operation not permitted')
        for mod, name in ((shutil, 'copystat'), (os, 'utime'), (os, 'chmod')):
            old = getattr(mod, name); setattr(mod, name, deny); self.addCleanup(setattr, mod, name, old)

    def test_copy_file_completes_without_metadata(self):
        pack_android.copy_file(self.src, self.dst)
        self.assertEqual(self.dst.read_bytes(), self.src.read_bytes())

    def test_current_copy_is_judged_by_size_not_time(self):
        pack_android.copy_file(self.src, self.dst)
        self.assertTrue(pack_android.same_copy(self.src, self.dst))
        self.dst.write_bytes(b'short')
        self.assertFalse(pack_android.same_copy(self.src, self.dst))
        self.assertFalse(pack_android.same_copy(self.src, self.dst.with_name('none')))


if __name__ == '__main__':
    unittest.main()
