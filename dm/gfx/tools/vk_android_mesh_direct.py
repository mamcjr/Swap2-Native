#!/usr/bin/env python3
# Direct mesh outputs for a library built before them (vk_android_mesh.direct_outputs, in the chain since 2026-11-15), as replay overrides: an agent tool for A/B runs
# on an older library without a new build (DM_VK_SPV_OVERRIDE). Not part of DMSH_VK_CHAIN.
# Why: the driver's statistics (DM_VK_PIPE_STATS) put Ocarina of Time's heavy mesh computes at 4-6 % ALU occupancy with 8-13 KB of workgroup memory per 32-thread
# group, almost all of it the Workgroup copy of the planes that vk_android_spv.defer_mesh_plane_stores adds (made 2026-11-12 so Turnip would not die compiling the store
# chain inlined at every call). Two steps on every MOD_MESH_COMPUTE module of a library:
#   A (undefer): the calls of mesh_plane_defer go back to mesh_plane_store, and the copy, its written mask and the clear / flush loops are removed;
#   B (direct): mesh_plane_store's chains of `if (plane == p) out.member[index]... = value` become one lookup in two Private tables (byte offset and stride of each plane,
#     from the output block's Offset / ArrayStride decorations; ~0 = a plane with no output, not stored) and one store into a uint[] view of the same output binding
#     (after DroidPlay 2e802839, MIT: each mesh output stored straight into the output buffer). The GL depth copy of planes 2 / 3 (mesh_position_zw) is kept.
# Measured on the POCO F7 Ultra (Adreno 830, 891.14), Ocarina of Time by Kokiri Forest's scarecrow, 2026-11-15 (docs/RESULTADOS-ETAPA1-2026-11-15.md, tenth step):
# library 18.7 / 18.5 fps (GPU 47.7 / 48.0 ms); A 21.4 / 21.6 (40.6), its code ~3.5x larger; A + B 22.7 / 22.5 (39.1 / 38.9), code near the original size, same picture.
# Usage: python vk_android_mesh_direct.py <dmsh_vk.lib> <out folder> [key ...]   (needs spirv-val: SPIRV_VAL or the NDK's shader-tools on PATH, and zstandard)
# On the phone: DM_VK_SPV_OVERRIDE=<the folder pushed there> (vka_shader.c spv_override).
import os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import vk_android_delta as delta, vk_android_mesh as vmesh
VK = delta.VK
VAL = os.environ.get('SPIRV_VAL', 'spirv-val')

def valid(data):
    with tempfile.NamedTemporaryFile(suffix='.spv', delete=False) as f: f.write(data); p = f.name
    try: r = subprocess.run([VAL, '--target-env', 'vulkan1.3', p], capture_output=True, text=True)
    finally: os.unlink(p)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


if __name__ == '__main__':
    import zstandard as zstd   # pyright: ignore[reportMissingImports]  # optional: only this agent tool needs it (the header's usage line)
    lib, out = sys.argv[1], sys.argv[2]; only = set(sys.argv[3:])
    os.makedirs(out, exist_ok=True)
    L = delta.Library(lib)
    n = done = bad = skip = 0
    for i in range(L.ne):
        key, e, binds, c1, mods = L.entry(i)
        if only and key not in only: continue
        byv = {v: (size, st, codec) for v, size, st, codec in mods}
        v = VK['MOD_MESH_COMPUTE']
        if v not in byv: continue
        n += 1
        data = delta.spirv(byv[v][1], byv[v][2], zstd)
        new = vmesh.direct_outputs(data)
        if new == data: skip += 1; continue
        ok, msg = valid(new)
        if not ok: bad += 1; print('[RED]', key, msg[:400]); continue
        done += 1
        open(os.path.join(out, f'{key}_{v}.spv'), 'wb').write(new)
    print(f'{n} task/mesh computes: {done} written (direct stores), {skip} untouched, {bad} refused; to {out}')
