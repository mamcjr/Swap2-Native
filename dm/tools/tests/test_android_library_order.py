"""Home's library order (android/app LibraryOrder.java with android/app/src/jvmtest's LibraryOrderJvmTest): by name, release date (the profile's release row)
or last start, each ascending or descending; a game with no value for the key comes last in both directions, ties by name. Also checks that every Switch 2
profile's release row is a YYYY-MM-DD date (build.gradle refuses another form). The JVM part is skipped without a JDK."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'android/app/src/main/java/dev/nxnative/android/LibraryOrder.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/LibraryOrderJvmTest.java'


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class Order(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='liborder_'))
        subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', str(cls.tmp / 'classes'), str(SRC), str(TEST)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def java(self, *a):
        r = subprocess.run(['java', '-Duser.language=en', '-cp', str(self.tmp / 'classes'), 'dev.nxnative.android.LibraryOrderJvmTest', *a], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip().split(' ', 1)[1]

    def order(self, id): return self.java('order', id).split(',')

    def test_ids(self):
        self.assertEqual(self.java('ids'), 'name_asc,name_desc,release_asc,release_desc,played_asc,played_desc true true')

    def test_name(self):   # case does not count
        self.assertEqual(self.order('name_asc'), ['apple', 'donkey', 'Mario', 'unknown', 'Yoshi', 'zelda'])
        self.assertEqual(self.order('name_desc'), ['zelda', 'Yoshi', 'unknown', 'Mario', 'donkey', 'apple'])

    def test_release_missing_last(self):
        self.assertEqual(self.order('release_asc'), ['apple', 'Mario', 'donkey', 'zelda', 'unknown', 'Yoshi'])
        self.assertEqual(self.order('release_desc'), ['zelda', 'donkey', 'apple', 'Mario', 'unknown', 'Yoshi'])

    def test_played_missing_last(self):
        self.assertEqual(self.order('played_asc'), ['apple', 'donkey', 'Yoshi', 'zelda', 'Mario', 'unknown'])
        self.assertEqual(self.order('played_desc'), ['zelda', 'Yoshi', 'apple', 'donkey', 'Mario', 'unknown'])


class Profiles(unittest.TestCase):
    def test_release_rows(self):
        for t in sorted((REPO / 'dm/games').glob('*/game.tsv')):
            for line in t.read_text(encoding='utf-8').splitlines():
                if line.startswith('release\t'):
                    self.assertRegex(line.split('\t')[1].strip(), r'^\d{4}-\d{2}-\d{2}$', t)


if __name__ == '__main__':
    unittest.main()
