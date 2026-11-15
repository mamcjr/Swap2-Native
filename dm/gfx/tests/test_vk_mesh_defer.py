"""vk_android_spv.py defer_mesh_plane_stores: a mesh program's plane stores into a workgroup copy, handed to the outputs by one loop after the body"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import vk_android_mesh as mesh
import vk_android_spv as spv

# The translator's shape (SpirvMeshEntry.cs): main sets the outputs, calls the body between two barriers; the body calls mesh_plane_store(word, value), which
# drops a word at or past the per-primitive base (128: one chunk of 32 vertices, 4 planes) and stores the others into vertex (w / 128) x 32 + w % 32, plane (w % 128) / 32
SOURCE = '''OpCapability Shader
OpCapability MeshShadingEXT
OpExtension "SPV_EXT_mesh_shader"
OpMemoryModel Logical GLSL450
OpEntryPoint MeshEXT %main "main" %lid %out0 %idx
OpExecutionMode %main LocalSize 32 1 1
OpExecutionMode %main OutputVertices 32
OpExecutionMode %main OutputPrimitivesEXT 32
OpExecutionMode %main OutputTrianglesEXT
OpName %main "main"
OpName %store "mesh_plane_store"
OpDecorate %lid BuiltIn LocalInvocationIndex
OpDecorate %out0 Location 0
OpDecorate %idx BuiltIn PrimitiveTriangleIndicesEXT
%void = OpTypeVoid
%bool = OpTypeBool
%u = OpTypeInt 32 0
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%uv3 = OpTypeVector %u 3
%c0 = OpConstant %u 0
%c2 = OpConstant %u 2
%c5 = OpConstant %u 5
%c31 = OpConstant %u 31
%c32 = OpConstant %u 32
%c128 = OpConstant %u 128
%c264 = OpConstant %u 264
%a32v4 = OpTypeArray %v4 %c32
%a32uv3 = OpTypeArray %uv3 %c32
%pin = OpTypePointer Input %u
%pout = OpTypePointer Output %a32v4
%pidx = OpTypePointer Output %a32uv3
%pof = OpTypePointer Output %float
%lid = OpVariable %pin Input
%out0 = OpVariable %pout Output
%idx = OpVariable %pidx Output
%fn = OpTypeFunction %void
%sfn = OpTypeFunction %void %u %u
%main = OpFunction %void None %fn
%me = OpLabel
OpSetMeshOutputsEXT %c32 %c32
OpControlBarrier %c2 %c2 %c264
%r = OpFunctionCall %void %body
OpControlBarrier %c2 %c2 %c264
OpReturn
OpFunctionEnd
%body = OpFunction %void None %fn
%be = OpLabel
%l = OpLoad %u %lid
CALLS
OpReturn
OpFunctionEnd
%store = OpFunction %void None %sfn
%w = OpFunctionParameter %u
%v = OpFunctionParameter %u
%se = OpLabel
%f = OpBitcast %float %v
%isv = OpULessThan %bool %w %c128
OpSelectionMerge %sm None
OpBranchConditional %isv %st %sm
%st = OpLabel
%rem = OpUMod %u %w %c128
%plane = OpShiftRightLogical %u %rem %c5
%q = OpUDiv %u %w %c128
%qi = OpIMul %u %q %c32
%lo = OpBitwiseAnd %u %rem %c31
%vi = OpIAdd %u %qi %lo
%p = OpAccessChain %pof %out0 %vi %plane
OpStore %p %f
OpBranch %sm
%sm = OpLabel
OpReturn
OpFunctionEnd
'''


def calls(n):   # n stores of the invocation's index: at word lid (plane 0), lid + 32 (plane 1) and, every third one, lid + 128 (past the planes: dropped)
    return '\n'.join(f'%w{k} = OpIAdd %u %l %c{(0, 32, 128)[k % 3]}\n%x{k} = OpFunctionCall %void %store %w{k} %l' for k in range(n))


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs SPIRV-Tools')
class DeferPlaneStores(unittest.TestCase):
    def build(self, n):
        with tempfile.TemporaryDirectory() as td:
            a, b = Path(td) / 'm.spvasm', Path(td) / 'm.spv'
            a.write_text(SOURCE.replace('CALLS', calls(n)))
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.3', str(a), '-o', str(b)], check=True, capture_output=True)
            return b.read_bytes()

    def valid(self, data):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'x.spv'; p.write_bytes(data)
            r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(p)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_few_calls_untouched(self):
        data = self.build(spv.MESH_DEFER_MIN_CALLS - 1)
        m = spv.Module(data)
        self.assertIsNone(spv.defer_mesh_plane_stores(m))
        self.assertEqual(m.data(), data)

    def test_deferred(self):
        n = spv.MESH_DEFER_MIN_CALLS + 2
        m = spv.Module(self.build(n))
        st = spv.defer_mesh_plane_stores(m)
        self.assertEqual(st, {'calls': n, 'words': 128, 'bytes': 4 * (128 + 4)})
        self.valid(m.data())
        store = next(i for i, s in m.names.items() if s == 'mesh_plane_store')
        defer = next(i for i, s in m.names.items() if s == 'mesh_plane_defer')
        flush = next(i for i, s in m.names.items() if s == 'mesh_plane_flush')
        clear = next(i for i, s in m.names.items() if s == 'mesh_plane_clear')
        called = [x[3] for x in m.ins if x[0] & 0xffff == spv.OP_FUNCTION_CALL]
        self.assertEqual(called.count(store), 1)   # (the flush's: the store chain is inlined once)
        self.assertEqual(called.count(defer), n)
        fa, fe = m.function_range(flush)
        self.assertTrue(any(x[0] & 0xffff == spv.OP_FUNCTION_CALL and x[3] == store for x in m.ins[fa:fe]))
        # main: clear, barrier, body, barrier, flush, barrier
        ma, me = m.function_range(m.entry_function())
        seq = [('call', x[3]) if x[0] & 0xffff == spv.OP_FUNCTION_CALL else 'barrier' for x in m.ins[ma:me] if x[0] & 0xffff in (spv.OP_FUNCTION_CALL, spv.OP_CONTROL_BARRIER)]
        self.assertEqual(seq[0], ('call', clear)); self.assertEqual(seq[1], 'barrier'); self.assertEqual(seq[3:], ['barrier', ('call', flush), 'barrier'])
        # the copy and the mask are workgroup variables of the entry point
        wg = [v for v, (_, sc) in m.vars.items() if sc == spv.SC_WORKGROUP]
        self.assertEqual(len(wg), 2); self.assertTrue(set(wg) <= set(m.interface()))
        # the deferred store keeps the word only below the planes' end and sets its bit atomically
        da, de = m.function_range(defer)
        ops = [x[0] & 0xffff for x in m.ins[da:de]]
        self.assertIn(spv.OP_ATOMIC_OR, ops); self.assertIn(spv.OP_ULESS_THAN, ops)
        # the compute fallback lowered from it is valid too
        cs, vs, _ = mesh.lower(m.data())
        self.valid(cs); self.valid(vs)

    def test_over_the_workgroup_budget_untouched(self):
        data = self.build(spv.MESH_DEFER_MIN_CALLS + 2)
        keep = spv.MESH_DEFER_MAX_BYTES
        try:
            spv.MESH_DEFER_MAX_BYTES = 4 * 128
            m = spv.Module(data)
            self.assertIsNone(spv.defer_mesh_plane_stores(m))
            self.assertEqual(m.data(), data)
        finally:
            spv.MESH_DEFER_MAX_BYTES = keep


if __name__ == '__main__':
    unittest.main()
