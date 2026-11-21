// A lighter FSR 1 EASU (fsr_easu.frag, ndsr_easu.comp; AMD's full one, ffx_fsr1.h FsrEasuF, is the DM_VK_EASU_FULL=1 variant). After the ideas in atyuwen's
// "Optimizing FSR", written anew from FSR 1 (not from that post's code):
// - the edge analysis runs once, on the luma bilinearly interpolated at the sample point, instead of at the 4 nearest texels and blended by their bilinear weights;
// - where that luma has no edge (|dir|^2 < 1/128) the result is the plain bilinear of the 4 nearest texels, without the 12 Lanczos weights;
// - the colour and luma math is mediump (RelaxedPrecision: fp16 where the driver does it); the coordinates stay highp.
// Still 12 texel fetches around the sample point and the deringing clamp to the 4 nearest texels. The includer defines EasuLightTap(ivec2 p): the colour of input
// texel p, kept inside the image. pp: the input pixel position (texel centres at integers) of this output pixel, EASU's AF2(ip) * con0.xy + con0.zw
void EasuLightW(inout mediump vec3 ac, inout mediump float aw, mediump vec2 off, mediump vec2 dir, mediump vec2 len2, mediump float lob, mediump float clp, mediump vec3 c) {
    mediump vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;
    mediump float d2 = min(dot(v, v), clp);
    mediump float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;
    wb *= wb; wa *= wa;
    mediump float w = (1.5625 * wb - 0.5625) * wa;
    ac += c * w; aw += w;
}
mediump float EasuLightLum(mediump vec3 c) { return c.g + 0.5 * (c.r + c.b); }
mediump vec3 EasuLight(highp vec2 pp) {
    highp vec2 fpf = floor(pp);
    mediump vec2 f = vec2(pp - fpf);
    ivec2 p = ivec2(fpf);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    mediump vec3 bC = EasuLightTap(p + ivec2(0, -1)), cC = EasuLightTap(p + ivec2(1, -1));
    mediump vec3 eC = EasuLightTap(p + ivec2(-1, 0)), fC = EasuLightTap(p), gC = EasuLightTap(p + ivec2(1, 0)), hC = EasuLightTap(p + ivec2(2, 0));
    mediump vec3 iC = EasuLightTap(p + ivec2(-1, 1)), jC = EasuLightTap(p + ivec2(0, 1)), kC = EasuLightTap(p + ivec2(1, 1)), lC = EasuLightTap(p + ivec2(2, 1));
    mediump vec3 nC = EasuLightTap(p + ivec2(0, 2)), oC = EasuLightTap(p + ivec2(1, 2));
    mediump float bL = EasuLightLum(bC), cL = EasuLightLum(cC), eL = EasuLightLum(eC), fL = EasuLightLum(fC), gL = EasuLightLum(gC), hL = EasuLightLum(hC);
    mediump float iL = EasuLightLum(iC), jL = EasuLightLum(jC), kL = EasuLightLum(kC), lL = EasuLightLum(lC), nL = EasuLightLum(nC), oL = EasuLightLum(oC);
    mediump vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    // The luma cross around the sample point (up, left, middle, right, down), each the bilinear of its 2x2 texels
    mediump float xU = dot(w, vec4(bL, cL, fL, gL)), xL = dot(w, vec4(eL, fL, iL, jL)), xM = dot(w, vec4(fL, gL, jL, kL));
    mediump float xR = dot(w, vec4(gL, hL, kL, lL)), xD = dot(w, vec4(jL, kL, nL, oL));
    mediump float dx = xR - xL, dy = xD - xU;
    mediump vec2 dir = vec2(dx, dy);
    mediump float dr = dot(dir, dir);
    mediump vec3 bil = fC * w.x + gC * w.y + jC * w.z + kC * w.w;
    if (dr < 1.0 / 128.0) return bil;
    mediump float lx = clamp(abs(dx) / max(max(abs(xR - xM), abs(xM - xL)), 1.0 / 1024.0), 0.0, 1.0);
    mediump float ly = clamp(abs(dy) / max(max(abs(xD - xM), abs(xM - xU)), 1.0 / 1024.0), 0.0, 1.0);
    mediump float len = (lx * lx + ly * ly) * 0.5; len *= len;
    dir *= inversesqrt(dr);
    mediump float stretch = 1.0 / max(abs(dir.x), abs(dir.y));
    mediump vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    mediump float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;
    mediump vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));
    mediump vec3 ac = vec3(0.0);
    mediump float aw = 0.0;
    EasuLightW(ac, aw, vec2(0.0, -1.0) - f, dir, len2, lob, clp, bC);
    EasuLightW(ac, aw, vec2(1.0, -1.0) - f, dir, len2, lob, clp, cC);
    EasuLightW(ac, aw, vec2(-1.0, 1.0) - f, dir, len2, lob, clp, iC);
    EasuLightW(ac, aw, vec2(0.0, 1.0) - f, dir, len2, lob, clp, jC);
    EasuLightW(ac, aw, vec2(0.0, 0.0) - f, dir, len2, lob, clp, fC);
    EasuLightW(ac, aw, vec2(-1.0, 0.0) - f, dir, len2, lob, clp, eC);
    EasuLightW(ac, aw, vec2(1.0, 1.0) - f, dir, len2, lob, clp, kC);
    EasuLightW(ac, aw, vec2(2.0, 1.0) - f, dir, len2, lob, clp, lC);
    EasuLightW(ac, aw, vec2(2.0, 0.0) - f, dir, len2, lob, clp, hC);
    EasuLightW(ac, aw, vec2(1.0, 0.0) - f, dir, len2, lob, clp, gC);
    EasuLightW(ac, aw, vec2(1.0, 2.0) - f, dir, len2, lob, clp, oC);
    EasuLightW(ac, aw, vec2(0.0, 2.0) - f, dir, len2, lob, clp, nC);
    // (weights summing to about 0: the bilinear result, as where there is no edge)
    return aw > 1.0 / 1024.0 ? min(mx, max(mn, ac / aw)) : bil;
}
