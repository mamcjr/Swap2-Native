# Snapdragon Game Super Resolution 2 (GSR2): notice

The three compute shaders here (`gsr2_convert.comp`, `gsr2_activate.comp`, `gsr2_upscale.comp`) and their SPIR-V (`vka_gsr2_spv.h`) are Qualcomm's
[Snapdragon Game Super Resolution 2](https://github.com/SnapdragonStudios/snapdragon-gsr), the 3-pass compute version (`sgsr/v2/include/glsl_3_pass_cs`, commit `d926f074`),
adapted for NVN2's DLSS stand-in by [Banana Native](https://github.com/moceure/BananaNative) (commits `86d75790` and `849d2fb3`, tag `eden/v0.3.45`; each shader's
header lists its changes) and taken here unchanged from Banana Native.

Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved. SPDX-License-Identifier: BSD-3-Clause. The license's full text, its conditions and
its disclaimer are in [LICENSE](LICENSE) and apply to the shaders in source form and to their SPIR-V in binary form (the Android host, `libnxhost.so`, carries the
SPIR-V; [android/NOTICE.md](../../../../android/NOTICE.md) repeats the notice for the APK).

Banana Native's own adaptations are MIT-licensed (Copyright (c) moceure and the Banana Native contributors), as is the rest of the host. The SPIR-V is rebuilt and
checked with `python3 dm/gfx/vk_android/fsr/tsr_spv.py --check` (glslc -O --target-env=vulkan1.1; the NDK's r28 and r30 give the same words).
