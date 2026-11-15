"""The launcher's own library search and Add game's dump check (LibraryScan.java, DumpCheck.identify, Nup.Incomplete) on the JVM: javac compiles them with
PackFind, Pack, Nup, Zstd, Profile, ShaderLibs, Switch2Layout, Frontend, DumpLayout, GameFlow, PackFacts, a stub BuildConfig (Donkey Kong Bananza's profile needing
update 458752), a stub android.os.StatFs and android/app/src/jvmtest's LibraryScanJvmTest, which makes its .nup fixtures with dm/tools/nup_pack.py (this
Python). The search: the games of the Switch2 folder and the library folder each once, which ones it checks (not on a card, not refused before, not removed,
not still being copied), its remembered marks; a base-only .nup beside its game's update .nup; a .nup cut short told apart from a damaged one; and the check
Add game now makes as Play does (another version, the base alone, an update-only .nup without RomFS, another build). Skipped without a JDK."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
APP = REPO / 'android/app/src'
MAIN = APP / 'main/java/dev/nxnative/android'
TEST = APP / 'jvmtest/java/dev/nxnative/android/LibraryScanJvmTest.java'
BUILD = 'aabbccddeeff00112233445566778899'
STUB = '''package dev.nxnative.android;
public final class BuildConfig {
    public static final String NX_PROFILES = "040074A01BF12000\\tDonkey Kong Bananza\\t22852\\tabcdef012345\\t458752 main=%s";
    public static final String NX_DMSH_VK_CHAIN = "c0ffee000001", NX_DMSH_VK_ABI = "abc123abc123", NX_DMSH_VK_ABI_COMPAT = "";
}
''' % BUILD
STATFS = '''package android.os;
public class StatFs {
    public StatFs(String path) {}
    public long getAvailableBytes() { return 1L << 40; }
}
'''


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class LibraryScanJvm(unittest.TestCase):
    def test_search_and_check(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'stub/android/os').mkdir(parents=True)
            (d / 'stub/BuildConfig.java').write_text(STUB)
            (d / 'stub/android/os/StatFs.java').write_text(STATFS)
            srcs = [MAIN / f'{c}.java' for c in ('LibraryScan', 'PackFind', 'Pack', 'Nup', 'Zstd', 'Profile', 'ShaderLibs', 'Switch2Layout', 'Frontend', 'DumpLayout', 'GameFlow', 'PackFacts')]
            srcs += [APP / 'ondevice/java/dev/nxnative/android/DumpCheck.java', TEST, d / 'stub/BuildConfig.java', d / 'stub/android/os/StatFs.java']
            r = subprocess.run(['javac', '-Xlint:all,-try', '-Werror', '-encoding', 'UTF-8', '-d', str(d / 'classes'), *map(str, srcs)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            (d / 'run').mkdir()
            r = subprocess.run(['java', '-cp', str(d / 'classes'), 'dev.nxnative.android.LibraryScanJvmTest', str(REPO), str(d / 'run'), sys.executable],
                               capture_output=True, text=True, encoding='utf-8')
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.startswith('ok '), r.stdout)


if __name__ == '__main__':
    unittest.main()
