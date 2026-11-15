#!/usr/bin/env python3
# Test loop for the task/mesh lowering (vk_android_mesh.py) without a new library: every task / mesh entry of a library (dmsh_vk.lib) is lowered again from its base
# module with the vk_android_mesh.py beside this file, and each compute / pulling-vertex module that comes out different from the library's is written as a replay
# override, <key>_<variant>.spv (vka_shader.c spv_override: DM_VK_SPV_OVERRIDE=<folder> on the phone takes them in place of the library's), after spirv-val.
# Usage: python vk_android_mesh_override.py <dmsh_vk.lib> <out folder> [--check]   (--check: only says whether the library's modules come out the same, the
#   lowering's determinism, and writes nothing). Needs spirv-val on PATH or in SPIRV_VAL (the NDK's shader-tools) and the zstandard module for zstd entries.
# Not part of the chain: an agent tool. A change found good this way still goes into the chain the usual way (dmsh_chain.py --header, a full library build).
import os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import vk_android_delta as delta, vk_android_mesh as mesh
VK = delta.VK


def spirv_val(data):
    with tempfile.NamedTemporaryFile(suffix='.spv', delete=False) as f: f.write(data); p = f.name
    try: r = subprocess.run([os.environ.get('SPIRV_VAL', 'spirv-val'), '--target-env', 'vulkan1.3', p], capture_output=True, text=True)
    finally: os.unlink(p)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def main():
    lib, out = sys.argv[1], sys.argv[2]; check = '--check' in sys.argv
    try: import zstandard as zstd
    except ImportError: zstd = None
    L = delta.Library(lib); os.makedirs(out, exist_ok=True)
    n = same = changed = bad = 0
    for i in range(L.ne):
        key, e, binds, c1, mods = L.entry(i)
        byv = {v: (size, st, codec) for v, size, st, codec in mods}
        if VK['MOD_MESH_COMPUTE'] not in byv: continue
        n += 1
        base = delta.spirv(byv[VK['MOD_BASE']][1], byv[VK['MOD_BASE']][2], zstd)
        cs, vs, _ = mesh.lower(base)
        for variant, new in ((VK['MOD_MESH_COMPUTE'], cs), (VK['MOD_MESH_VERTEX'], vs)):
            if new is None: continue
            old = delta.spirv(byv[variant][1], byv[variant][2], zstd) if variant in byv else None
            if old == new: same += 1; continue
            changed += 1
            if check: continue
            ok, msg = spirv_val(new)
            if not ok: bad += 1; print(f'[RED] {key} variant {variant}: spirv-val: {msg[:300]}'); continue
            with open(os.path.join(out, f'{key}_{variant}.spv'), 'wb') as f: f.write(new)
    print(f'{n} task/mesh entries: {same} modules the same as the library, {changed} different ({bad} refused by spirv-val){"" if check else ", written to " + out}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
