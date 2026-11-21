"""The temporal upscalers' host shaders (NVN2's DLSS stand-in, dm/gfx/vk_android/vka_dlss.c and vka_fsr3.c, ported from Banana Native eden/v0.3.45; the temporal path in
agl's NDSR slot, vka_ndsr.c DM_VK_NDSR=gsr2).

- The SPIR-V headers match their shaders, word for word (dm/gfx/vk_android/fsr/tsr_spv.py --check: fsr/ndsr_tsr.comp and ndsr_noop.comp, gsr2/*.comp, fsr/dlss_*.comp).
- The FSR 3.1 headers (dm/gfx/vk_android/fsr3/*.h) pass spirv-val; with FIDELITYFX_SDK=<a FidelityFX-SDK checkout at v1.1.4> also dm/tools/fsr3_shaders.py --check
  (byte for byte with glslang 16.6.0; without it the script fetches the SDK from GitHub, which this test does not do).
- The motion pass on a real Vulkan driver (dm/gfx/vk_android/selftest/tsr_lvp.c, made for Mesa's lavapipe; Linux or WSL with gcc and libvulkan.so.1): the GPU's
  camera motion equals the CPU reference (vka_tsr_cam.h) for both depth modes and window origins, the reversed depth, the colour pass.
Each part skips when its tools are missing."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
VK = REPO / 'dm' / 'gfx' / 'vk_android'
sys.path.insert(0, str(VK / 'fsr'))
import ndsr_spv  # noqa: E402


class Headers(unittest.TestCase):
    def test_tsr_gsr2_dlss_headers(self):
        if not ndsr_spv.tool('glslc') or not ndsr_spv.tool('spirv-val'):
            self.skipTest('needs glslc and spirv-val')
        r = subprocess.run([sys.executable, str(VK / 'fsr' / 'tsr_spv.py'), '--check'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_fsr3_headers_validate(self):
        val = ndsr_spv.tool('spirv-val')
        if not val:
            self.skipTest('needs spirv-val')
        heads = sorted((VK / 'fsr3').glob('*.h'))
        self.assertEqual(len(heads), 11)
        with tempfile.TemporaryDirectory() as d:
            for h in heads:
                m = re.search(r'const\s+uint32_t\s+(\w+)\s*\[\]\s*=\s*\{([^}]*)\}', h.read_text(encoding='utf-8'))
                self.assertIsNotNone(m, h.name)
                w = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(2))]
                spv = os.path.join(d, h.stem + '.spv')
                Path(spv).write_bytes(b''.join(x.to_bytes(4, 'little') for x in w))
                r = subprocess.run([val, '--target-env', 'vulkan1.1', spv], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, f'{h.name}: {r.stderr}')

    def test_fsr3_sdk_check(self):
        sdk = os.environ.get('FIDELITYFX_SDK')
        if not sdk:
            self.skipTest('FIDELITYFX_SDK=<FidelityFX-SDK v1.1.4 checkout> runs fsr3_shaders.py --check')
        r = subprocess.run([sys.executable, str(REPO / 'dm' / 'tools' / 'fsr3_shaders.py'), '--check', '--sdk', sdk], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


class Lavapipe(unittest.TestCase):
    def test_motion_pass(self):
        cc = shutil.which('gcc') or shutil.which('cc')
        if sys.platform != 'linux' or not cc:
            self.skipTest('needs Linux (or WSL) with gcc')
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, 'tsr_lvp')
            subprocess.run([cc, '-O1', '-I', str(REPO / 'dm/third_party/vulkan-headers/include'), '-I', str(VK), str(VK / 'selftest/tsr_lvp.c'), '-o', exe, '-ldl', '-lm'], check=True)
            env = dict(os.environ)
            lvp = [p for p in ('/usr/share/vulkan/icd.d/lvp_icd.json', '/usr/share/vulkan/icd.d/lvp_icd.x86_64.json') if os.path.exists(p)]
            if lvp and 'VK_ICD_FILENAMES' not in env:
                env['VK_ICD_FILENAMES'] = lvp[0]
            r = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=300)
            if r.stdout.startswith('SKIP'):
                self.skipTest(r.stdout.strip())
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
