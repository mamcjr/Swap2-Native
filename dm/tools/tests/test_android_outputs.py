"""The on-device Prepare's split outputs (docs/reference/packing.md "The split refused on shared storage (HyperOS)"): Outputs.java written in place with a
completion marker instead of a renamed folder, and an older build's renamed split/ still accepted, on the JVM (android/app/src/jvmtest's OutputsJvmTest;
skipped without a JDK)."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
APP = REPO / 'android/app/src'
SRC = [APP / 'ondevice/java/dev/nxnative/android/Outputs.java']
TEST = APP / 'jvmtest/java/dev/nxnative/android/OutputsJvmTest.java'
JOB = APP / 'ondevice/java/dev/nxnative/android/Job.java'
SERVICE = APP / 'ondevice/java/dev/nxnative/android/PackService.java'


def names(src, pattern):
    m = re.search(pattern, src, re.S)
    assert m, pattern
    return re.findall(r'"([^"]+)"', m.group(1))


class SplitOutputs(unittest.TestCase):
    def test_names_agree(self):
        # Job.SPLIT_OUTPUTS (what an older build's renamed split/ must hold to count) names what PackService's split step writes, and the JVM test checks
        # the same list: an output added to the split and not to the list would let an older split/ without it pass as complete
        job = names(JOB.read_text(encoding='utf-8'), r'SPLIT_OUTPUTS\s*=\s*\{([^}]*)\}')
        test = names(TEST.read_text(encoding='utf-8'), r'String\[\]\s+SPLIT\s*=\s*\{([^}]*)\}')
        step = names(SERVICE.read_text(encoding='utf-8'), r'case "split":.*?argv\s*=\s*new String\[\]\s*\{(.*?)\};')
        self.assertEqual(job, test)
        self.assertEqual(sorted(n.rstrip('/') for n in job), sorted(step))


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class Outputs(unittest.TestCase):
    def test_jvm(self):
        with tempfile.TemporaryDirectory(prefix='outputs_') as tmp:
            classes, work = Path(tmp, 'classes'), Path(tmp, 'work')
            classes.mkdir()
            work.mkdir()
            subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', str(classes), *map(str, SRC), str(TEST)], check=True, capture_output=True, text=True)
            r = subprocess.run(['java', '-cp', str(classes), 'dev.nxnative.android.OutputsJvmTest', str(work)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('all ok', r.stdout)


if __name__ == '__main__':
    unittest.main()
