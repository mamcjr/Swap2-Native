"""vk_android_spv.py promote_locals: a bool variable is promoted only when it is read in its store's own block (Qualcomm 891.14 refused compute pipelines with many
bool values alive across blocks); ints and floats are promoted across blocks as before"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import vk_android_spv as spv

SOURCE = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%bool = OpTypeBool
%u = OpTypeInt 32 0
%pb = OpTypePointer Function %bool
%pu = OpTypePointer Function %u
%fn = OpTypeFunction %void
%one = OpConstant %u 1
%two = OpConstant %u 2
%main = OpFunction %void None %fn
%entry = OpLabel
%bvar = OpVariable %pb Function
%bsame = OpVariable %pb Function
%uvar = OpVariable %pu Function
%c = OpULessThan %bool %one %two
OpStore %bvar %c
OpStore %bsame %c
OpStore %uvar %two
%s = OpLoad %bool %bsame
%x = OpSelect %u %s %one %two
OpBranch %next
%next = OpLabel
%b = OpLoad %bool %bvar
%v = OpLoad %u %uvar
%y = OpSelect %u %b %v %x
OpReturn
OpFunctionEnd
'''


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs SPIRV-Tools')
class PromoteBool(unittest.TestCase):
    def test_bool_across_blocks_stays(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = Path(td) / 'm.spvasm', Path(td) / 'm.spv'
            a.write_text(SOURCE)
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.3', str(a), '-o', str(b)], check=True, capture_output=True)
            m = spv.Module(b.read_bytes())
            vs = [x[2] for x in m.ins if x[0] & 0xffff == spv.OP_VARIABLE]
            bvar, bsame, uvar = vs
            spv.promote_locals(m)
            left = {x[2] for x in m.ins if x[0] & 0xffff == spv.OP_VARIABLE}
            self.assertIn(bvar, left)        # a bool read in another block stays a variable
            self.assertNotIn(bsame, left)    # a bool read in its own block is promoted
            self.assertNotIn(uvar, left)     # an int read in another block is promoted
            b.write_bytes(m.data())
            r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(b)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
