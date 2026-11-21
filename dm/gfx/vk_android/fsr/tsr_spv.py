#!/usr/bin/env python3
"""The SPIR-V headers of the temporal upscalers, checked against their shaders:
- ../vka_ndsr_tsr_spv.h (written by this script): fsr/ndsr_tsr.comp and fsr/ndsr_noop.comp, the temporal path in agl's NDSR slot (vka_ndsr.c, DM_VK_NDSR=gsr2);
- ../gsr2/vka_gsr2_spv.h and ../vka_dlss_fsr.h (Banana Native's, eden/v0.3.45, committed as they came): gsr2/gsr2_{convert,activate,upscale}.comp and
  fsr/dlss_{prep,easu,rcas,copy}.comp, NVN2's DLSS stand-in (vka_dlss.c). Their words are compared (the headers keep upstream's layout).
Each is compiled with glslc -O --target-env=vulkan1.1 (the NDK's r28 and r30 give the same words) and checked with spirv-val --target-env vulkan1.1.
Needs glslc and spirv-val: on PATH, or the NDK's shader-tools (ANDROID_NDK_HOME, or ~/android-ndk-*). --check: write nothing, exit 1 when a header differs
(dm/tools/tests/test_ndsr_tsr.py). The FSR 3.1 headers (../fsr3) have their own script, dm/tools/fsr3_shaders.py."""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..'))
sys.path.insert(0, HERE)
from ndsr_spv import tool  # noqa: E402

FSR1 = os.path.join(REPO, 'dm', 'third_party', 'fidelityfx-fsr1')
OUT = os.path.join(HERE, '..', 'vka_ndsr_tsr_spv.h')
# (header, array, shader, include folders); the first header is the one this script writes
SETS = [
    (OUT, [('vka_ndsr_tsr', 'ndsr_tsr.comp'), ('vka_ndsr_noop', 'ndsr_noop.comp')], HERE, []),
    (os.path.join(HERE, '..', 'gsr2', 'vka_gsr2_spv.h'), [('vka_gsr2_convert', 'gsr2_convert.comp'), ('vka_gsr2_activate', 'gsr2_activate.comp'), ('vka_gsr2_upscale', 'gsr2_upscale.comp')],
     os.path.join(HERE, '..', 'gsr2'), []),
    (os.path.join(HERE, '..', 'vka_dlss_fsr.h'), [('vka_dlss_prep', 'dlss_prep.comp'), ('vka_dlss_easu', 'dlss_easu.comp'), ('vka_dlss_rcas', 'dlss_rcas.comp'), ('vka_dlss_copy', 'dlss_copy.comp')],
     HERE, [FSR1]),
]


def words(glslc, val, src, incs, tmp):
    spv = os.path.join(tmp, os.path.basename(src) + '.spv')
    subprocess.run([glslc, '-O', '--target-env=vulkan1.1'] + ['-I' + i for i in incs] + [src, '-o', spv], check=True)
    subprocess.run([val, '--target-env', 'vulkan1.1', spv], check=True)
    b = open(spv, 'rb').read()
    return [int.from_bytes(b[i:i + 4], 'little') for i in range(0, len(b), 4)]


def arrays(path):
    t = open(path, encoding='utf-8').read() if os.path.exists(path) else ''
    return {m.group(1): [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(2))]
            for m in re.finditer(r'const\s+uint32_t\s+(\w+)\s*\[\]\s*=\s*\{([^}]*)\}', t)}


def tsr_header(compiled):
    out = ['// SPIR-V of the temporal path in agl\'s NDSR slot (vka_ndsr.c, DM_VK_NDSR=gsr2): the colour and camera-motion pass and the no-op network dispatch. Made from',
           '// dm/gfx/vk_android/fsr/ndsr_{tsr,noop}.comp by dm/gfx/vk_android/fsr/tsr_spv.py (the NDK\'s glslc; r28 and r30 give the same words), checked with spirv-val:',
           '//   glslc -O --target-env=vulkan1.1 -mfmt=num dm/gfx/vk_android/fsr/<shader> -o -',
           '// Do not edit',
           '#pragma once', '#include <stdint.h>']
    for name, w in compiled:
        out.append(f'static const uint32_t {name}[] = {{')
        out += [','.join(f'0x{x:08x}' for x in w[i:i + 4]) + ',' for i in range(0, len(w), 4)]
        out.append('};')
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check', action='store_true', help='exit 1 when a header is stale')
    a = ap.parse_args()
    glslc, val = tool('glslc'), tool('spirv-val')
    if not glslc or not val:
        print('tsr_spv: glslc and spirv-val are needed (PATH, ANDROID_NDK_HOME or ~/android-ndk-*)', file=sys.stderr)
        return 2
    stale = 0
    with tempfile.TemporaryDirectory() as tmp:
        for k, (hdr, items, folder, incs) in enumerate(SETS):
            try:
                compiled = [(n, words(glslc, val, os.path.join(folder, s), incs, tmp)) for n, s in items]
            except subprocess.CalledProcessError as e:
                print(f'tsr_spv: {e}', file=sys.stderr)
                return 2
            rel = os.path.relpath(hdr, REPO).replace(os.sep, '/')
            if k == 0 and not a.check:
                text = tsr_header(compiled)
                old = open(hdr, newline='').read() if os.path.exists(hdr) else ''
                if old != text:
                    with open(hdr, 'w', newline='\n') as f:
                        f.write(text)
                    print(f'tsr_spv: wrote {rel}')
                continue
            have = arrays(hdr)
            for n, w in compiled:
                same = have.get(n) == w
                stale += not same
                print(f"tsr_spv: {'same' if same else 'DIFFERS'}  {rel} {n} ({len(w)} words)")
    if stale:
        print(f'tsr_spv: {stale} array(s) differ from their shaders' + ('' if a.check else ' (the upstream headers are not rewritten: regenerate them by hand)'), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
