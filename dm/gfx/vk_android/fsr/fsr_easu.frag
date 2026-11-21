#version 450
#extension GL_GOOGLE_include_directive : require
precision highp float;   // (the default, said for the mediump in fsr_easu_light.h)
precision highp int;
// FSR 1 EASU: the game's picture (tex, the crop the constants name) scaled to the intermediate image, one output pixel per fragment.
// FULL (constant_id 0; vka_present.c, DM_VK_EASU_FULL): 0 = the lighter EASU (fsr_easu_light.h, 12 texel fetches), 1 = AMD's full one (FsrEasuF, 12 gathers)
layout(constant_id = 0) const int FULL = 0;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform P { uvec4 con0, con1, con2, con3; } pc;
layout(location = 0) out vec4 o;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(tex, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(tex, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(tex, p, 2); }
#include "ffx_fsr1.h"
// The texel kept inside the texture (g_last: its last texel), as the clamp-to-edge sampler of the gathers
ivec2 g_last;
mediump vec3 EasuLightTap(ivec2 p) { return texelFetch(tex, clamp(p, ivec2(0), g_last), 0).rgb; }
#include "fsr_easu_light.h"
void main() {
    AF3 c;
    if (FULL != 0) FsrEasuF(c, AU2(gl_FragCoord.xy), pc.con0, pc.con1, pc.con2, pc.con3);
    else { g_last = textureSize(tex, 0) - 1; c = EasuLight(vec2(AU2(gl_FragCoord.xy)) * uintBitsToFloat(pc.con0.xy) + uintBitsToFloat(pc.con0.zw)); }
    o = vec4(c, 1.0);
}
