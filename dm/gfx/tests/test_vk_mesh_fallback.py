"""Validate the actual task/mesh lowering and generate its game-independent GPU fixtures."""
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/gfx/tools'))
import vk_android_mesh as mesh
import vk_android_spv as spv


def flatten_fixture_payload(data):
    """GLSL's one-member payload block -> the translator's plain uint-array payload ABI."""
    m = spv.Module(data)
    if 1 not in m.caps: m.ins.insert(0, mesh.ins(17, 1))  # The translator declares Shader explicitly.
    for v, (ptr, sc) in list(m.vars.items()):
        if sc != 5402: continue
        st = m.types[ptr][3]; ar = m.types[st][2]
        assert m.op(st) == 30 and len(m.types[st]) == 3 and m.op(ar) == 28
        np = m.add_type(32, 5402, ar)
        for i, x in enumerate(m.ins):
            op = x[0] & 65535
            if op == 59 and x[2] == v: x[1] = np
            if op in (65, 66) and x[3] == v:
                assert m.consts[x[4]][1] == [0]
                m.ins[i] = mesh.ins(op, x[1], x[2], v, *x[5:])
    a = next(i for i, x in enumerate(m.ins) if x[0] & 65535 == 54)
    m.ins = [x for x in m.ins[:a] if x[0] & 65535 != 59] + [x for x in m.ins[:a] if x[0] & 65535 == 59] + m.ins[a:]
    return m.data()


def fixtures(directory):
    modules, metadata = {}, {}
    for stage in ('task', 'mesh', 'frag'):
        src = ROOT / 'dm/gfx/vk_android/selftest' / ('mesh_fallback.' + stage)
        out = directory / (stage + '.spv')
        subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', '-o', str(out), str(src)], check=True, capture_output=True)
        data = out.read_bytes()
        if stage != 'frag':
            data = flatten_fixture_payload(data)
            cs, vs, meta = mesh.lower(data)
            modules[stage + '_compute'] = cs; metadata[stage] = meta
            if vs: modules['mesh_vertex'] = vs
        else: modules['fragment'] = data
    return modules, metadata


@unittest.skipUnless(shutil.which('glslangValidator') and shutil.which('spirv-val'), 'needs glslangValidator and spirv-val')
class MeshFallback(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mesh-fallback-')
        cls.path = Path(cls.temp.name); cls.modules, cls.meta = fixtures(cls.path)
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def test_modules_validate(self):
        for name, data in self.modules.items():
            with self.subTest(module=name):
                p = self.path / (name + '.spv'); p.write_bytes(data)
                r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(p)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)

    def test_compute_has_no_mesh_interface(self):
        for name in ('task_compute', 'mesh_compute'):
            m = spv.Module(self.modules[name])
            self.assertEqual(m.model, 'cs')
            self.assertNotIn(5283, m.caps)
            self.assertNotIn('SPV_EXT_mesh_shader', m.exts)
            self.assertFalse(any(sc in (3, 5402) for _, sc in m.vars.values()))
            self.assertFalse(any(x[0] & 65535 in (5294, 5295) for x in m.ins))

    def test_generated_invocation_temporaries_are_function_local(self):
        for name in ('task_compute', 'mesh_compute'):
            m = spv.Module(self.modules[name])
            self.assertFalse(any(sc == 6 for _, sc in m.vars.values()))

    def test_native_style_member_annotations_without_debug_names(self):
        src = ROOT / 'dm/gfx/vk_android/selftest/mesh_fallback.mesh'
        raw = self.path / 'native-style.spv'
        subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', '-o', str(raw), str(src)], check=True, capture_output=True)
        m = spv.Module(flatten_fixture_payload(raw.read_bytes()))
        m.ins = [x for x in m.ins if x[0] & 65535 not in (5, 6)]
        a = next(i for i, x in enumerate(m.ins) if x[0] & 65535 in (71, 72))
        members = [x for x in m.ins if x[0] & 65535 == 72]
        m.ins = [x for x in m.ins if x[0] & 65535 != 72]
        m.ins[a:a] = members
        cs, vs, _ = mesh.lower(m.data())
        for name, data in [('native-cs', cs), ('native-vs', vs)]:
            out = self.path / (name + '.spv'); out.write_bytes(data)
            r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(out)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_vertex_preserves_primitive_interpolation_and_depth(self):
        m = spv.Module(self.modules['mesh_vertex'])
        self.assertEqual(m.model, 'vs')
        self.assertNotIn(5271, {d for v in m.decos.values() for d in v})
        self.assertTrue(any(14 in d and 30 in d for d in m.decos.values()))
        self.assertTrue(any(d.get(1) == [0] for d in m.decos.values()))

    def test_metadata_and_storage_limits(self):
        self.assertEqual(self.meta['task']['payload_words'], 64)
        self.assertEqual(self.meta['mesh']['max_primitives'], 256)
        self.assertEqual(self.meta['mesh']['topology'], 3)
        self.assertGreater(self.meta['mesh']['stride'], 8192)
        self.assertEqual(self.meta['mesh']['stride'] % 16, 0)

    def test_points_and_culling(self):
        src = ROOT / 'dm/gfx/vk_android/selftest/mesh_fallback_points.mesh'
        raw = self.path / 'points-native.spv'
        subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', '-o', str(raw), str(src)], check=True, capture_output=True)
        # Match the translator's separate BuiltIn output arrays rather than GLSL's
        # per-primitive interface block. Production mesh shaders use CullPrimitive
        # as a direct Boolean output array.
        m = spv.Module(raw.read_bytes())
        for v, (ptr, sc) in list(m.vars.items()):
            if sc != 3: continue
            ar=m.pointee(ptr)[1]
            if m.op(ar)!=28 or m.op(m.types[ar][2])!=30: continue
            st=m.types[ar][2]
            if len(m.types[st])!=3 or m.op(m.types[st][2])!=20: continue
            a=m.add_type(28,m.types[st][2],m.types[ar][3]); p=m.add_type(32,3,a)
            for i,x in enumerate(m.ins):
                if x[0]&65535==59 and x[2]==v: x[1]=p
                if x[0]&65535 in (65,66) and x[3]==v:
                    self.assertEqual(m.consts[x[5]][1],[0]);m.ins[i]=mesh.ins(x[0]&65535,x[1],x[2],v,x[4])
            mesh.Build(m).deco(v,11,5299)
            m.ins=[x for x in m.ins if not (x[0]&65535==72 and x[1]==st)]
        a=next(i for i,x in enumerate(m.ins) if x[0]&65535==54)
        m.ins=[x for x in m.ins[:a] if x[0]&65535!=59]+[x for x in m.ins[:a] if x[0]&65535==59]+m.ins[a:]
        if 1 not in m.caps: m.ins.insert(0,mesh.ins(17,1))
        cs,vs,meta=mesh.lower(m.data())
        self.assertEqual(meta['topology'],1)
        self.assertEqual(meta['max_primitives'],16)
        before,dead,live=_vertex_branches(spv.Module(vs))
        self.assertEqual(len(before),2)   # the count and the cull flag: the rest is behind the gate
        self.assertEqual(len(live),3)     # the point's index, position and point size
        self.assertEqual(len(dead),2)     # a clipped position and point size 1
        for name,data in [('points-cs',cs),('points-vs',vs)]:
            out=self.path/(name+'.spv');out.write_bytes(data)
            r=subprocess.run(['spirv-val','--target-env','vulkan1.3',str(out)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)


def _out_group(m):
    """(OUT variable, group struct type, index of its trailing primitive-count member, runtime array stride)"""
    out = next(v for v, d in m.decos.items() if d.get(33) == [mesh.OUT] and v in m.vars)
    block = m.pointee(m.vars[out][0])[1]; rt = m.types[block][2]; group = m.types[rt][2]
    return out, group, len(m.types[group]) - 3, m.decos[rt][6][0]


def _functions(m):
    """{function id: its instructions, OpFunction to OpFunctionEnd}"""
    out, fn = {}, None
    for x in m.ins:
        op = x[0] & 65535
        if op == 54: fn = x[2]; out[fn] = []
        if fn is not None: out[fn].append(x)
        if op == 56: fn = None
    return out


def _count_helper(m):
    """(the function storing the group's primitive count, its stores, the constant arguments of its calls in order)"""
    out, group, last, _ = _out_group(m)
    ptrs = {x[2] for x in m.ins if x[0] & 65535 == 65 and x[3] == out and len(x) == 7 and m.consts.get(x[6], (0, [None]))[1] == [last]}
    owners = {f: [x for x in body if x[0] & 65535 == 62 and x[1] in ptrs] for f, body in _functions(m).items()}
    owners = {f: s for f, s in owners.items() if s}
    if len(owners) != 1: return None, [], []
    fid, stores = next(iter(owners.items()))
    args = [m.consts[x[4]][1][0] if x[4] in m.consts else None for x in m.ins if x[0] & 65535 == 57 and x[3] == fid]
    return fid, stores, args


def _vertex_branches(m):
    """(storage-buffer loads before the gate, the dead branch's instructions, storage-buffer loads in the live branch)"""
    body = _functions(m)[m.entry_function()]
    sb = {i for i, x in m.types.items() if x[0] & 65535 == 32 and x[2] == 12}
    chains = {x[2] for x in body if x[0] & 65535 == 65 and x[1] in sb}
    loads = lambda xs: [x for x in xs if x[0] & 65535 == 61 and x[3] in chains]
    k = next(i for i, x in enumerate(body) if x[0] & 65535 == 250)
    live, dead = body[k][2], body[k][3]
    at = {x[1]: i for i, x in enumerate(body) if x[0] & 65535 == 248}
    end = lambda i: next(j for j in range(i + 1, len(body)) if body[j][0] & 65535 in (249, 250, 253))
    return loads(body[:k]), body[at[dead] + 1:end(at[dead])], loads(body[at[live] + 1:end(at[live])])


def _validate(test, path, data):
    path.write_bytes(data)
    r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(path)], capture_output=True, text=True)
    test.assertEqual(r.returncode, 0, r.stderr)


@unittest.skipUnless(shutil.which('glslangValidator') and shutil.which('spirv-val'), 'needs glslangValidator and spirv-val')
class PrimitiveCount(unittest.TestCase):
    """OpSetMeshOutputsEXT's primitive count kept in the group's output, and the pulling stage's gate."""
    SRC = '''#version 460
#extension GL_EXT_mesh_shader : require
layout(local_size_x=32) in;
layout(triangles,max_vertices=64,max_primitives=126) out;
out gl_MeshPerVertexEXT { vec4 gl_Position; } gl_MeshVerticesEXT[];
layout(location=0) out vec4 color[];
layout(location=1) perprimitiveEXT out vec4 tint[];
void main() {
    uint n = gl_WorkGroupID.x % 126u;
    SetMeshOutputsEXT(64, n);
    for (uint i = gl_LocalInvocationIndex; i < 64u; i += 32u) { gl_MeshVerticesEXT[i].gl_Position = vec4(float(i)); color[i] = vec4(1); }
    for (uint i = gl_LocalInvocationIndex; i < n; i += 32u) { gl_PrimitiveTriangleIndicesEXT[i] = uvec3(i % 64u, (i + 1u) % 64u, (i + 2u) % 64u); tint[i] = vec4(0, 1, 0, 1); }
}
'''

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mesh-count-')
        cls.path = Path(cls.temp.name)
        src = cls.path / 'count.mesh'; src.write_text(cls.SRC)
        raw = cls.path / 'count.spv'
        subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', '-o', str(raw), str(src)], check=True, capture_output=True)
        m = spv.Module(raw.read_bytes())
        if 1 not in m.caps: m.ins.insert(0, mesh.ins(17, 1))
        cls.native = m.data()
        cls.cs, cls.vs, cls.meta = mesh.lower(cls.native)
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def test_validates(self):
        _validate(self, self.path / 'count-cs.spv', self.cs)
        _validate(self, self.path / 'count-vs.spv', self.vs)

    def test_count_is_stored_by_the_first_invocation(self):
        c = spv.Module(self.cs)
        self.assertFalse(any(x[0] & 65535 == 5295 for x in c.ins))
        out, group, last, stride = _out_group(c)
        self.assertEqual(c.types[group][2 + last], c.find_type(21, 32, 0))   # a trailing uint
        self.assertEqual(stride, self.meta['stride'])
        fid, stores, args = _count_helper(c)
        self.assertEqual(len(stores), 1)
        self.assertEqual(args, [0, None])   # 0 at the entry, then the shader's runtime count
        body = _functions(c)[fid]
        lid = next(v for v, d in c.decos.items() if d.get(11) == [29] and v in c.vars)
        loads = {x[2] for x in body if x[0] & 65535 == 61 and x[3] == lid}
        cond = next(x for x in body if x[0] & 65535 == 170 and loads & set(x[3:]))
        branch = next(x for x in body if x[0] & 65535 == 250)
        self.assertEqual(branch[1], cond[2])
        self.assertLess(body.index(branch), body.index(stores[0]))
        # The zero comes before anything else of the entry that could set the count.
        entry = _functions(c)[c.entry_function()]
        calls = [x for x in entry if x[0] & 65535 == 57 and x[3] == fid]
        self.assertEqual(c.consts[calls[0][4]][1], [0])

    def test_entry_without_set_mesh_outputs_emits_nothing(self):
        m = spv.Module(self.native); m.ins = [x for x in m.ins if x[0] & 65535 != 5295]
        cs, vs, _ = mesh.lower(m.data())
        _validate(self, self.path / 'unset-cs.spv', cs); _validate(self, self.path / 'unset-vs.spv', vs)
        self.assertEqual(_count_helper(spv.Module(cs))[2], [0])

    def test_vertex_gate(self):
        v = spv.Module(self.vs)
        before, dead, live = _vertex_branches(v)
        self.assertEqual(len(before), 2)   # the count and the primitive's indices
        self.assertEqual(len(live), 3)   # position, the per-vertex and the per-primitive generic
        self.assertTrue(dead and all(x[0] & 65535 == 62 for x in dead))
        pos = next(x for x in dead if v.decos.get(x[1], {}).get(11) == [0])
        cc = next(x for x in v.ins if x[0] & 65535 == 44 and x[2] == pos[2])
        self.assertEqual([v.consts[c][1][0] for c in cc[3:]], [0x40000000] * 3 + [0x3f800000])
        self.assertTrue(any(x[0] & 65535 == 176 for x in v.ins))   # prim < count
        self.assertTrue(any(x[0] & 65535 == 166 for x in v.ins))   # i0 != i1 || i1 != i2
        # Live primitives keep their interpolation and invariance.
        self.assertTrue(any(14 in d and d.get(30) == [1] for d in v.decos.values()))
        self.assertTrue(any(18 in d and d.get(11) == [0] for d in v.decos.values()))


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs SPIRV-Tools')
class LocalForwarding(unittest.TestCase):
    def module(self, body, decoration='', helper=''):
        source = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
''' + decoration + '''
%void = OpTypeVoid
%u = OpTypeInt 32 0
%p = OpTypePointer Function %u
%fn = OpTypeFunction %void
%hfn = OpTypeFunction %void %p
%one = OpConstant %u 1
%two = OpConstant %u 2
%main = OpFunction %void None %fn
%entry = OpLabel
%local = OpVariable %p Function
''' + body + '''
OpReturn
OpFunctionEnd
''' + helper
        with tempfile.TemporaryDirectory() as td:
            a, b = Path(td)/'test.spvasm', Path(td)/'test.spv'
            a.write_text(source)
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.3', str(a), '-o', str(b)], check=True, capture_output=True)
            m = spv.Module(b.read_bytes())
            before = {x[2]: x[3] for x in m.ins if x[0]&65535 == 61}
            counts = mesh.promote_function_locals(m)
            b.write_bytes(m.data())
            result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(b)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return m, before, counts

    def test_latest_store_is_forwarded(self):
        m, before, counts = self.module('OpStore %local %one\n%a = OpLoad %u %local\nOpStore %local %two\n%b = OpLoad %u %local')
        self.assertEqual(counts, (2, 1))
        copies = [x for x in m.ins if x[0]&65535 == 83 and x[2] in before]
        self.assertEqual([m.consts[x[3]][1] for x in copies], [[1], [2]])

    def test_read_after_label_keeps_memory(self):
        m, before, counts = self.module('OpStore %local %one\nOpBranch %next\n%next = OpLabel\n%a = OpLoad %u %local')
        self.assertEqual(counts, (0, 0))
        self.assertTrue(any(x[0]&65535 == 61 and x[2] in before for x in m.ins))

    def test_pointer_call_keeps_memory(self):
        helper = '''%helper = OpFunction %void None %hfn
%ptr = OpFunctionParameter %p
%he = OpLabel
OpStore %ptr %two
OpReturn
OpFunctionEnd
'''
        _, _, counts = self.module('OpStore %local %one\n%c = OpFunctionCall %void %helper %local\n%a = OpLoad %u %local', helper=helper)
        self.assertEqual(counts, (0, 0))

    def test_volatile_access_keeps_memory(self):
        for decoration, suffix in [('', ' Volatile'), ('OpDecorate %local Volatile', '')]:
            with self.subTest(decoration=decoration):
                _, _, counts = self.module('OpStore %local %one\n%a = OpLoad %u %local'+suffix, decoration)
                self.assertEqual(counts, (0, 0))


def header(path):
    with tempfile.TemporaryDirectory(prefix='mesh-fixtures-') as td:
        mods, meta = fixtures(Path(td)); lines = ['// Generated game-independent task/mesh fallback fixtures.', '#pragma once']
        for name, data in mods.items():
            w = struct.unpack('<%dI' % (len(data) // 4), data)
            lines.append('static const uint32_t SPV_MESH_' + name.upper() + '[] = {')
            lines.extend('    ' + ', '.join(hex(x) for x in w[i:i+8]) + ',' for i in range(0, len(w), 8)); lines.append('};')
        lines.append('#define MESH_TEST_STRIDE ' + str(meta['mesh']['stride']))
        Path(path).write_text('\n'.join(lines) + '\n', encoding='utf-8')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--header': header(sys.argv[2])
    else: unittest.main()
