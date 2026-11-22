// Aim beam (laser1): a thin red line from the right hand along where shots
// go (bonehook.cpp), drawn into each eye's image just before its Present, so
// it is in 3D like the rest of the view. Drawn on top of everything (no
// depth test). We save the few states we change and put them back. Shown on foot only, while motion aiming
// is on.
#include <stdio.h>
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <math.h>

#include "fc3vr.h"

namespace {

const char kShader[] =
    "cbuffer C : register(b0) { float4 corner[4]; float4 color; float4 win[2]; float4 dz; float4 occ; };\n"
    // c4icons1 (DrGonzaga 12 Nov, his picture): the chest throwable icons 1-4 from a texture (4 tiles,
    // premultiplied, mips); corner[2].w > 0.5 on those quads when the texture is there.
    "Texture2D icons : register(t0); SamplerState smp : register(s0);\n"
    "struct V { float4 p : SV_Position; float a : A; float2 uv : U; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.p = float4(corner[id].xy, dz[id], 1); o.a = corner[id].z;\n"
    "  o.uv = float2(id & 1, id >> 1) * 2 - 1; return o; }\n"
    // dot1: corner[0].w > 1.5 marks the hit glow: a bright core (radius corner[2].w of the
    // quad) inside a soft halo (strength corner[1].w), round, facing the viewer.
    // holster2: corner[0].w > 2.5 = a holster mark, corner[3].w = which: 0 sphere, 1 grenade,
    // 2 C4, 3 molotov, 4 mine (shapes drawn from distance functions; dark disc behind icons).
    "float sdBox(float2 p, float2 b) { float2 d = abs(p) - b; return length(max(d, 0)) + min(max(d.x, d.y), 0); }\n"
    "float sdEll(float2 p, float2 r) { return (length(p / r) - 1) * min(r.x, r.y); }\n"
    "float sdFlame(float2 p) {\n"
    "  float b = length(p - float2(0, -0.3)) - 0.44;\n"
    "  float2 q = float2(p.x - 0.1 * saturate((p.y + 0.1) / 0.95), p.y);\n"
    "  float tip = max(max(-0.25 - q.y, q.y - 0.88), (abs(q.x) - 0.46 * pow(saturate((0.88 - q.y) / 1.13), 0.75)) * 0.75);\n"
    "  float2 l = float2(abs(p.x) - 0.33, p.y);\n"
    "  float side = max(max(-0.05 - l.y, l.y - 0.42), (abs(l.x) - 0.12 * pow(saturate((0.42 - l.y) / 0.47), 0.8)) * 0.75);\n"
    "  return min(min(b, tip), side); }\n"
    // slowmo1: icon 10 (26 neon) = a clock: the face, four hour marks, two hands (ten past ten);
    // distance to the strokes' centre lines, *h = the centre pin
    "float sgC(float2 p, float2 a, float2 b) { float2 pa = p - a, ba = b - a; return length(pa - ba * saturate(dot(pa, ba) / dot(ba, ba))); }\n"
    "float clockD(float2 p, out float h) {\n"
    "  float d = abs(length(p) - 0.7);\n"
    "  float2 q = abs(p); d = min(d, min(sgC(q, float2(0, 0.5), float2(0, 0.6)), sgC(q, float2(0.5, 0), float2(0.6, 0))));\n"
    "  d = min(d, min(sgC(p, float2(0, 0), float2(0.27, 0.32)), sgC(p, float2(0, 0), float2(-0.2, 0.12))));\n"
    "  h = length(p) - 0.07; return d; }\n"
    // neon1 (DrGonzaga 12 Nov, Blood Dragon: "fit the neon future aesthetic": clear centres, neon rings, neon icons).
    // Ids 16+ = the neon set: 16 a wire globe (outer ring + equator + meridian: reads as a 3D sphere),
    // 24 a plain ring, 17..23 the icons above as glowing outlines. Lines are bright, a soft glow around
    // them, nothing in between.
    "float nl(float d, float w) { return 1 - smoothstep(w, w + 0.025, abs(d)); }\n"
    "float ng(float d) { return exp(-abs(d) * 8); }\n"
    "float4 neon(float2 uv, float id) {\n"
    "  float r = length(uv), ink, gl, hot = 0;\n"
    "  if (id < 0.5) {\n"
    "    float d0 = r - 0.84, d1 = (length(uv / float2(0.84, 0.28)) - 1) * 0.28, d2 = (length(uv / float2(0.28, 0.84)) - 1) * 0.28;\n"
    "    ink = max(nl(d0, 0.03), 0.6 * max(nl(d1, 0.014), nl(d2, 0.014)));\n"
    "    gl = max(ng(d0), 0.35 * max(ng(d1 * 2), ng(d2 * 2)));\n"
    "  } else if (id > 9.5) { float h = 0; float d = clockD(uv / 0.82, h) * 0.82; h *= 0.82;\n"  // slowmo1
    "    ink = nl(d, 0.022); gl = ng(d * 1.3); hot = max(nl(h, 0.018), 0.5 * ng(h * 1.6));\n"
    "  } else if (id > 7.5) { ink = nl(r - 0.86, 0.03); gl = ng(r - 0.86); }\n"
    "  else {\n"
    "    float2 p = uv / 0.82; float d, h = 9;\n"
    "    if (id < 1.5) d = min(min(sdEll(p - float2(0, -0.15), float2(0.42, 0.5)), sdBox(p - float2(0, 0.42), float2(0.14, 0.1))),\n"
    "                      sdBox(p - float2(0.22, 0.38), float2(0.05, 0.2)));\n"
    "    else if (id < 2.5) { d = sdBox(p, float2(0.56, 0.28)) - 0.06; h = length(p - float2(0.42, 0.12)) - 0.08; }\n"
    "    else if (id < 3.5) { d = min(sdBox(p - float2(0, -0.28), float2(0.22, 0.36)) - 0.08, sdBox(p - float2(0, 0.26), float2(0.08, 0.2)));\n"
    "                         h = sdEll(p - float2(0, 0.66), float2(0.13, 0.2)); }\n"
    "    else if (id < 4.5) d = min(sdEll(p - float2(0, -0.12), float2(0.66, 0.24)), sdEll(p - float2(0, 0.1), float2(0.24, 0.17)));\n"
    "    else if (id < 5.5) { float2 q = float2(abs(p.x), p.y);\n"
    "      d = min(max(0.32 - q.y, dot(q - float2(0.24, 0.32), float2(0.928, 0.371))), sdBox(p - float2(0, -0.26), float2(0.065, 0.6))); }\n"
    "    else if (id < 6.5) { d = sdFlame(p); float2 c0 = float2(0, -0.62); h = sdFlame((p - c0) / 0.45 + c0) * 0.45; }\n"
    "    else { float a = atan2(p.y, p.x) + 1.5708, sp = a * 10 / 6.28318, i = floor(sp), f = frac(sp);\n"
    "      float hh = frac(sin((i - 10 * floor(i / 10)) * 12.9898) * 43758.55), t = pow(1 - abs(f * 2 - 1), 1.4);\n"
    "      d = (length(p) - (0.42 + (0.34 + 0.2 * hh) * t)) * 0.6; h = length(p) - 0.14; }\n"
    "    d *= 0.82; h *= 0.82;\n"
    "    ink = nl(d, 0.022); gl = ng(d * 1.3);\n"
    "    hot = max(nl(h, 0.018), 0.5 * ng(h * 1.6));\n"
    "  }\n"
    "  float3 c = lerp(color.rgb, float3(1, 1, 1), ink * 0.45);\n"
    "  c = lerp(c, float3(1.0, 0.35, 0.75), saturate(hot) * (1 - ink));\n"
    "  float a = saturate(color.a * 2.5) * saturate(max(max(ink, hot), 0.5 * gl)) * (1 - smoothstep(0.93, 1.0, r));\n"
    "  return float4(c, a); }\n"
    "float4 mark(float2 uv, float id, float2 gx, float2 gy) {\n"
    "  if (((id > 16.5 && id < 20.5) || (id > 24.5 && id < 26.5)) && corner[2].w > 0.5) id -= 16;\n"  // bd28: the throwable pictures stay pictures in the neon set
    "  if (id > 15.5) return neon(uv, id - 16);\n"
    "  float r = length(uv);\n"
    // heart1: a fifth tile, the cyber heart, icon id 9 (25 in the neon set); slowmo2: a sixth, the clock, id 10 (26)
    "  if (corner[2].w > 0.5 && ((id > 0.5 && id < 4.5) || (id > 8.5 && id < 10.5))) {\n"
    "    const float s = 0.8; float2 tc = float2(0.5 + uv.x * 0.5 / s, 0.5 - uv.y * 0.5 / s), k = float2(0.1 / s, -0.5 / s);\n"
    "    float tile = id > 9.5 ? 5 : id > 8.5 ? 4 : id - 1;\n"
    "    float4 t = icons.SampleGrad(smp, float2((tile + clamp(tc.x, 0.004, 0.996)) / 6, saturate(tc.y)), gx * float2(k.x * 5 / 6, k.y), gy * float2(k.x * 5 / 6, k.y));\n"
    "    if (any(abs(uv) > s)) t = 0;\n"
    "    float bg = (1 - smoothstep(0.9, 1.0, r)) * 0.15, oa = t.a + bg * (1 - t.a);\n"
    "    float3 pm = t.rgb + float3(0.05, 0.05, 0.06) * bg * (1 - t.a);\n"
    "    return float4(pm / max(oa, 1e-4), oa * color.a); }\n"
    // holster6 (DrGonzaga 10 Nov): back to a flat see-through disc (the 3D shading removed),
    // one even colour at the slider's opacity, edge smoothed over the last few percent.
    "  if (id < 0.5) return float4(color.rgb, color.a * (1 - smoothstep(0.92, 1.0, r)));\n"
    "  if (id > 9.5 && id < 10.5) { float h = 0; float d = clockD(uv, h) - 0.055;\n"  // slowmo1: the clock, filled strokes
    "    float o = 1 - smoothstep(-0.025, 0.025, d), hf = 1 - smoothstep(-0.025, 0.025, h);\n"
    "    float3 c = lerp(lerp(float3(0.05, 0.05, 0.06), color.rgb, o), float3(1.0, 0.35, 0.1), hf);\n"
    "    return float4(c, color.a * max((1 - smoothstep(0.9, 1.0, r)) * 0.15, max(o, hf))); }\n"
    "  float d, hot = 9;\n"
    "  if (id < 1.5) d = min(min(sdEll(uv - float2(0, -0.15), float2(0.42, 0.5)), sdBox(uv - float2(0, 0.42), float2(0.14, 0.1))),\n"
    "                    sdBox(uv - float2(0.22, 0.38), float2(0.05, 0.2)));\n"
    "  else if (id < 2.5) { d = sdBox(uv, float2(0.56, 0.28)) - 0.06; d = max(d, 0.045 - abs(abs(uv.x) - 0.24));\n"
    "                       hot = length(uv - float2(0.42, 0.12)) - 0.08; }\n"
    "  else if (id < 3.5) { d = min(sdBox(uv - float2(0, -0.28), float2(0.22, 0.36)) - 0.08, sdBox(uv - float2(0, 0.26), float2(0.08, 0.2)));\n"
    "                       hot = sdEll(uv - float2(0, 0.66), float2(0.13, 0.2)); }\n"
    "  else if (id < 4.5) d = min(sdEll(uv - float2(0, -0.12), float2(0.66, 0.24)), sdEll(uv - float2(0, 0.1), float2(0.24, 0.17)));\n"
    // quiver1 (DrGonzaga 11 Nov): 5 arrowhead, 6 flame (hot core), 7 spiky explosion (hot core)
    // quiver14 (DrGonzaga 11 Nov): the arrow with a smaller head, as tall as the flame and the burst.
    "  else if (id < 5.5) { float2 q = float2(abs(uv.x), uv.y);\n"
    "    float tri = max(0.32 - q.y, dot(q - float2(0.24, 0.32), float2(0.928, 0.371)));\n"
    "    d = min(tri, sdBox(uv - float2(0, -0.26), float2(0.065, 0.6))); }\n"
    // quiver14 (DrGonzaga 11 Nov, his picture): a cartoon flame: round base, a tip leaning a little,
    // a small tongue each side; red-orange outside, orange, a yellow drop low in the middle.
    "  else if (id < 6.5) { float2 c0 = float2(0, -0.62);\n"
    "    float o = 1 - smoothstep(-0.02, 0.02, sdFlame(uv)), m = 1 - smoothstep(-0.02, 0.02, sdFlame((uv - c0) / 0.68 + c0) * 0.68);\n"
    "    float y = 1 - smoothstep(-0.02, 0.02, sdFlame((uv - c0) / 0.38 + c0) * 0.38);\n"
    "    float3 c = lerp(lerp(float3(1.0, 0.3, 0.08), float3(1.0, 0.58, 0.1), m), float3(1.0, 0.86, 0.25), y);\n"
    "    return float4(lerp(float3(0.05, 0.05, 0.06), c, o), color.a * max((1 - smoothstep(0.9, 1.0, r)) * 0.15, o)); }\n"
    // quiver14 (DrGonzaga 11 Nov: a spiky explosion, like a cartoon burst): sharp spikes of uneven
    // length (10), red outside, then orange, yellow and a pale core.
    "  else { float a = atan2(uv.y, uv.x) + 1.5708, sp = a * 10 / 6.28318, i = floor(sp), f = frac(sp);\n"
    "    float h = frac(sin((i - 10 * floor(i / 10)) * 12.9898) * 43758.55), t = 1 - abs(f * 2 - 1); t = pow(t, 1.4);\n"
    "    float R = 0.42 + (0.34 + 0.2 * h) * t, e = 0.02;\n"
    "    float o = 1 - smoothstep(-e, e, (r - R) * 0.6), m = 1 - smoothstep(-e, e, (r - R * 0.74) * 0.6);\n"
    "    float y = 1 - smoothstep(-e, e, (r - R * 0.5) * 0.6), k = 1 - smoothstep(-e, e, (r - 0.1 - 0.12 * t) * 0.6);\n"
    "    float3 c = lerp(lerp(lerp(color.rgb, float3(1.0, 0.5, 0.08), m), float3(1.0, 0.8, 0.22), y), float3(1.0, 0.97, 0.7), k);\n"
    "    return float4(lerp(float3(0.05, 0.05, 0.06), c, o), color.a * max((1 - smoothstep(0.9, 1.0, r)) * 0.15, o)); }\n"
    "  float fill = 1 - smoothstep(-0.025, 0.025, d), hf = 1 - smoothstep(-0.025, 0.025, hot);\n"
    "  float bg = (1 - smoothstep(0.9, 1.0, r)) * 0.15;\n"
    "  float3 c = lerp(float3(0.05, 0.05, 0.06), color.rgb, fill);\n"
    "  c = lerp(c, float3(1.0, 0.35, 0.1), hf);\n"
    "  return float4(c, color.a * max(bg, max(fill, hf))); }\n"
    // sight3 (DrGonzaga 10 Nov): sight reticles, corner[0].w > 3.5. Units: minutes of angle / reticle size.
    // corner[3].w style (1 holo ring, 2 chevron, 3 cross, 4 triangle), corner[2].w quad half-size,
    // corner[1].w one pixel, corner[3].z line half-width, corner[0].z softness, corner[1/2].z gun up on screen.
    // color.a above 1 = brighter than full (up to 2x): the colour is pushed up and towards white.
    "float sdSeg(float2 p, float2 a, float2 b) { float2 pa = p - a, ba = b - a; float h = saturate(dot(pa, ba) / dot(ba, ba)); return length(pa - ba * h); }\n"
    // wrist1m: mark 8 = a number 0-99 (corner[1].w), seven-segment strokes (floats only).
    "float is(float n, float k) { return abs(n - k) < 0.5 ? 1.0 : 0.0; }\n"
    "float dig(float2 p, float n) {\n"
    "  const float w = 0.2, h = 0.5; float d = 9;\n"
    "  float a = 1 - is(n, 1) - is(n, 4), b = 1 - is(n, 5) - is(n, 6), c = 1 - is(n, 2), e = is(n, 0) + is(n, 2) + is(n, 6) + is(n, 8);\n"
    "  float dd = 1 - is(n, 1) - is(n, 4) - is(n, 7), f = 1 - is(n, 1) - is(n, 2) - is(n, 3) - is(n, 7), g = 1 - is(n, 0) - is(n, 1) - is(n, 7);\n"
    "  d = min(d, sdSeg(p, float2(-w, h), float2(w, h)) + (1 - a) * 9);\n"
    "  d = min(d, sdSeg(p, float2(w, h), float2(w, 0)) + (1 - b) * 9);\n"
    "  d = min(d, sdSeg(p, float2(w, 0), float2(w, -h)) + (1 - c) * 9);\n"
    "  d = min(d, sdSeg(p, float2(-w, -h), float2(w, -h)) + (1 - dd) * 9);\n"
    "  d = min(d, sdSeg(p, float2(-w, 0), float2(-w, -h)) + (1 - e) * 9);\n"
    "  d = min(d, sdSeg(p, float2(-w, h), float2(-w, 0)) + (1 - f) * 9);\n"
    "  d = min(d, sdSeg(p, float2(-w, 0), float2(w, 0)) + (1 - g) * 9);\n"
    "  return d; }\n"
    "float numd(float2 uv, float v) {\n"
    "  float t = floor(v / 10 + 0.01), d;\n"
    // ammo2: three digits (100 to 999) for the ammo numbers
    "  if (v > 99.5) { float hh = floor(v / 100 + 0.001), r2 = v - hh * 100, t2 = floor(r2 / 10 + 0.01);\n"
    "    d = min(dig(uv + float2(0.62, 0), hh), min(dig(uv, t2), dig(uv - float2(0.62, 0), r2 - t2 * 10))); }\n"
    "  else if (v > 9.5) d = min(dig(uv + float2(0.36, 0), t), dig(uv - float2(0.36, 0), v - t * 10)); else d = dig(uv, v);\n"
    "  return d; }\n"
    // numstyle1 (DrGonzaga 17 Nov: "a few number style options, more digital"; numcol1: "option for colour", all styles use color.rgb): corner[1].w = style * 10000 + the number.
    // 0 as before, 1 digital (numstyle2, below), 2 neon tube (pink, glowing,
    // no panel), 3 dot matrix (amber dots), 4 green CRT (phosphor glow with scan lines). Built only with NUMSTYLES
    // defined; without it (if that compile fails) the plain numbers come back.
    "#ifdef NUMSTYLES\n"
    "float sgA(float2 p) { return max(max(p.y - 0.5, 0.37 - p.y), (abs(p.x) - p.y + 0.2) * 0.7071); }\n"
    "float sgB(float2 p) { return max(max(p.x - 0.3, 0.17 - p.x), max((p.y - p.x - 0.2) * 0.7071, (0.3 - p.x - p.y) * 0.7071)); }\n"
    "float sgG(float2 p) { return max(abs(p.y) - 0.065, (abs(p.x) + abs(p.y) - 0.3) * 0.7071); }\n"
    "float sgc(float d) { return 1 - smoothstep(-0.034, -0.01, d); }\n"
    // x = lit, y = all segments (the dark ones); digit box 0.6 x 1.0, segments 0.13 thick
    "float2 segcov(float2 p, float n) {\n"
    "  float a = 1 - is(n, 1) - is(n, 4), b = 1 - is(n, 5) - is(n, 6), c = 1 - is(n, 2), e = is(n, 0) + is(n, 2) + is(n, 6) + is(n, 8);\n"
    "  float dd = 1 - is(n, 1) - is(n, 4) - is(n, 7), f = 1 - is(n, 1) - is(n, 2) - is(n, 3), g = 1 - is(n, 0) - is(n, 1) - is(n, 7);\n"
    "  float ca = sgc(sgA(p)), cd = sgc(sgA(float2(p.x, -p.y))), cb = sgc(sgB(p)), cc = sgc(sgB(float2(p.x, -p.y)));\n"
    "  float cf = sgc(sgB(float2(-p.x, p.y))), ce = sgc(sgB(-p)), cg = sgc(sgG(p));\n"
    "  float on = max(max(max(ca * a, cb * b), max(cc * c, cd * dd)), max(max(ce * e, cf * f), cg * g));\n"
    "  float al = max(max(max(ca, cb), max(cc, cd)), max(max(ce, cf), cg));\n"
    "  return float2(on, al); }\n"
    "float numbox(float2 uv) { float2 q = abs(uv) - float2(0.8, 0.66); return length(max(q, 0)) + min(max(q.x, q.y), 0) - 0.12; }\n"
    "float4 num(float2 uv) {\n"
    "  float v = floor(corner[1].w + 0.5), s = floor(v / 10000 + 0.0001); v = v - s * 10000;\n"
    "  float g8 = v > 99.5 ? 888 : v > 9.5 ? 88 : 8;\n"
    "  float pan = 1 - smoothstep(-0.02, 0.02, numbox(uv));\n"
    // numcol2 (DrGonzaga 17 Nov: "options for background colours and number colours"): win[0] = panel rgb and
    // opacity, win[1].rgb = unlit segments, win[1].w = 1 when given (the wrist counts)
    "  bool cu = win[1].w > 0.5; float3 BG = cu ? win[0].rgb : float3(0, 0, 0), OFF = cu ? win[1].rgb : float3(0.15, 0.15, 0.16);\n"
    "  float PA = cu ? win[0].w : 0.9;\n"
    // numstyle2 (DrGonzaga 17 Nov, his picture): bold upright digital digits, aqua, chunky segments with mitred
    // ends and thin gaps, the unlit ones dark grey, on black. Each segment is a convex shape (half planes).
    "  if (s > 0.5 && s < 1.5) { float on = 0, gh = 0, n = v, k = v > 99.5 ? 3 : v > 9.5 ? 2 : 1;\n"
    "    float sp = 0.7, x0 = (k - 1) * 0.5 * sp;\n"
    "    for (int j = 0; j < 3; ++j) { if (j < k) {\n"
    "      float pw = pow(10.0, k - 1 - j), dv = floor(n / pw + 0.001); n = n - dv * pw;\n"
    "      float2 r = segcov(uv + float2(x0 - j * sp, 0), dv); on = max(on, r.x); gh = max(gh, r.y); } }\n"
    "    float3 c = lerp(lerp(BG, OFF, gh), color.rgb, on);\n"
    "    return float4(c, color.a * max(pan * PA, max(on, gh))); }\n"
    "  if (s > 1.5 && s < 2.5) { float d = numd(uv, v);\n"
    "    float core = 1 - smoothstep(0.02, 0.045, d), tube = 1 - smoothstep(0.045, 0.075, d), glow = exp(-max(d, 0) * 11) * 0.75;\n"
    "    float3 c = lerp(color.rgb, lerp(color.rgb, float3(1, 1, 1), 0.75), core);\n"
    "    float il = saturate(max(tube, glow)), pa = cu ? pan * PA : 0;\n"
    "    return float4(lerp(BG, c, il), color.a * max(pa, il)); }\n"
    "  if (s > 2.5 && s < 3.5) { const float cell = 0.125; float2 q = (floor(uv / cell) + 0.5) * cell;\n"
    "    float lit = numd(q, v) < 0.085 ? 1.0 : 0.0, dot1 = 1 - smoothstep(cell * 0.3, cell * 0.42, length(uv - q));\n"
    "    float3 c = lerp(BG, lerp(OFF, color.rgb, lit), dot1);\n"
    "    return float4(c, color.a * max(pan * PA, dot1 * max(lit, 0.6))); }\n"
    "  if (s > 3.5) { float d = numd(uv, v);\n"
    "    float ink = 1 - smoothstep(0.05, 0.09, d), glow = exp(-max(d, 0) * 14) * 0.5;\n"
    "    float scan = 0.62 + 0.38 * sin(uv.y * 42.0);\n"
    "    float3 c = lerp(BG, color.rgb, saturate(max(ink, glow) * scan));\n"
    "    return float4(c, color.a * max(pan * PA, saturate(max(ink, glow)))); }\n"
    "  float d = numd(uv, v), ink = 1 - smoothstep(0.07, 0.12, d), bg = (1 - smoothstep(0.85, 1.0, length(uv))) * 0.55;\n"
    "  return float4(lerp(float3(0.03, 0.03, 0.04), color.rgb, ink), color.a * max(ink, bg)); }\n"
    "#else\n"
    "float4 num(float2 uv) {\n"
    "  float v = floor(corner[1].w + 0.5), d = numd(uv, v);\n"
    "  float ink = 1 - smoothstep(0.07, 0.12, d), bg = (1 - smoothstep(0.85, 1.0, length(uv))) * 0.55;\n"
    "  return float4(lerp(float3(0.03, 0.03, 0.04), color.rgb, ink), color.a * max(ink, bg)); }\n"
    "#endif\n"
    "float4 ret(float2 uv) {\n"
    "  float st = corner[3].w, px = corner[1].w, hw = max(corner[3].z, px * 0.6), soft = corner[0].z;\n"
    "  float2 q = uv * corner[2].w, u = float2(corner[1].z, corner[2].z);\n"
    "  float2 p = float2(q.x * u.y - q.y * u.x, q.x * u.x + q.y * u.y);\n"
    "  float d, dt = length(p) - hw * 1.6, d2 = 1e9;\n"
    "  if (st < 1.5) { d = abs(length(p) - 32.5);\n"
    "    d = min(d, min(sdSeg(p, float2(0, 28), float2(0, 32.5)), sdSeg(p, float2(0, -28), float2(0, -32.5))));\n"
    "    d = min(d, min(sdSeg(p, float2(28, 0), float2(32.5, 0)), sdSeg(p, float2(-28, 0), float2(-32.5, 0))));\n"
    "    d = min(d - hw, dt); }\n"
    "  else if (st < 2.5) d = min(sdSeg(p, float2(0, 0), float2(-5, -6)), sdSeg(p, float2(0, 0), float2(5, -6))) - hw;\n"
    "  else if (st < 3.5) { d = min(min(sdSeg(p, float2(0, 3), float2(0, 15)), sdSeg(p, float2(0, -3), float2(0, -15))),\n"
    "                              min(sdSeg(p, float2(3, 0), float2(15, 0)), sdSeg(p, float2(-3, 0), float2(-15, 0)))) - hw;\n"
    "    d = min(d, dt); }\n"
    "  else if (st < 4.5) d = max(max(-p.y - 4.5, dot(p, float2(0.874, 0.486))), dot(p, float2(-0.874, 0.486)));\n"
    // sight6 (DrGonzaga 10 Nov): bow pin sight. A dim round housing; four pins come in from the right,
    // each ending in a bright bead: the top one at the aim point, the others lower for longer shots.
    "  else if (st < 5.5) { d = 1e9; for (int k = 0; k < 4; ++k) { float y = -7.0 * k;\n"
    "      d = min(d, min(sdSeg(p, float2(1.5, y), float2(23, y)) - hw * 0.6, length(p - float2(0, y)) - hw * 2.0)); }\n"
    // sight7 (DrGonzaga 11 Nov): a second, wider ring (1.5x) to find the sight at arm's length.
    // sight8 (DrGonzaga 11 Nov): and a third, widest ring (3x).
    "    d2 = min(min(abs(length(p) - 24), abs(length(p) - 36)), abs(length(p) - 72)) - hw * 0.8; }\n"
    // future1 (DrGonzaga 12 Nov, Blood Dragon): four futuristic reticles. 6 target lock (corner brackets and a
    // dot), 7 tri-prong, 8 hex with side ticks, 9 cyber ring (three arcs, a small inner ring, a faint outer one).
    "  else if (st < 6.5) { float2 a = abs(p);\n"
    "    d = min(min(sdSeg(a, float2(20, 20), float2(20, 11)), sdSeg(a, float2(20, 20), float2(11, 20))) - hw, dt); }\n"
    "  else if (st < 7.5) { d = min(sdSeg(p, float2(0, -7), float2(0, -24)),\n"
    "      min(sdSeg(p, float2(6.06, 3.5), float2(20.8, 12)), sdSeg(p, float2(-6.06, 3.5), float2(-20.8, 12)))) - hw;\n"
    "    d = min(d, dt); }\n"
    "  else if (st < 8.5) { float2 a = abs(p);\n"
    "    d = abs(max(a.x * 0.866 + a.y * 0.5, a.y) - 24) - hw;\n"
    "    d = min(min(d, sdSeg(a, float2(31, 0), float2(38, 0)) - hw), dt); }\n"
    "  else { float rr = length(p), g = abs(frac(atan2(p.x, p.y) / 2.0944 + 0.5) - 0.5);\n"
    "    d = g > 0.07 ? abs(rr - 28) - hw : 1e9;\n"
    "    d = min(min(d, abs(rr - 5) - hw * 0.7), length(p) - hw);\n"
    "    d2 = abs(rr - 40) - hw * 0.6; }\n"
    "  float ink = max(1 - smoothstep(-px * 0.5, px * 0.5, d), 0.5 * (1 - smoothstep(-px * 0.5, px * 0.5, d2)));\n"
    "  d = min(d, d2);\n"
    "  float edge = 1 - smoothstep(0.8, 1.0, max(abs(uv.x), abs(uv.y)));\n"
    "  float glow = exp(-max(d, 0) / (px * 2 + hw * 3)) * soft * 0.5 * edge;\n"
    // sight4: window coordinates (edge = 1) at this pixel, from the quad's corners (win[0].xy at the centre,
    // win[0].zw per uv.x, win[1].xy per uv.y); win[1].z 1 = rectangle; win[1].w = 1 + where the fade starts.
    "  float2 w = win[0].xy + uv.x * win[0].zw + uv.y * win[1].xy;\n"
    "  float wr = win[1].z > 0.5 ? max(abs(w.x), abs(w.y)) : length(w), f0 = win[1].w - 1;\n"
    "  float wf = win[1].w > 0.5 ? 1 - saturate((wr - f0) / max(1 - f0, 0.001)) : 1;\n"
    "  float k = max(color.a, 1), al = min(color.a, 1) * wf;\n"
    "  return float4(saturate(lerp(color.rgb * k, 1, ink * 0.25 * k)), al * saturate(max(ink, glow * k))); }\n"
    "float4 ps(V i) : SV_Target {\n"
    "  if (corner[0].w > 3.5) return ret(i.uv);\n"
    "  if (corner[0].w > 2.5 && corner[3].w > 7.5 && corner[3].w < 8.5) return num(i.uv);\n"
    "  if (corner[0].w > 2.5) return mark(i.uv, corner[3].w, ddx(i.uv), ddy(i.uv));\n"
    "  if (corner[0].w < 1.5) return float4(color.rgb, color.a * i.a);\n"
    "  float r = length(i.uv), c = corner[2].w;\n"
    // soft1 (DrGonzaga 6 Nov, softer): the core fades out over its whole radius and the
    // halo is a gaussian taken to 0 at the quad's edge, so there is no visible rim.
    "  float core = 1 - smoothstep(c * 0.15, c * 1.25, r);\n"
    "  float halo = saturate((exp(-r * r * 5) - 0.0067) / 0.9933) * corner[1].w;\n"
    // sight3: color.a above 1 = up to twice as bright.
    "  float k = max(color.a, 1), al = min(color.a, 1);\n"
    "  return float4(saturate(lerp(color.rgb * k, 1, core * core * 0.3 * k)), al * saturate(max(core, halo * k))); }\n";

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                   UINT, UINT, ID3DBlob **, ID3DBlob **);

struct Cb {
    float corner[4][4];
    float color[4];
    float win[2][4];  // sight4: the sight window across the quad (reticles fade at its edge, per pixel)
    float dz[4];      // occl2: each corner's depth in the game's depth buffer (0 = in front of everything)
    float occ[4];     // occl2: x = samples in the depth texture (0 = no test); set to 1 by the caller to ask for the test
};

bool g_failed;
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
ID3D11VertexShader *g_vs;
ID3D11PixelShader *g_ps;
ID3D11Buffer *g_cb;
ID3D11RasterizerState *g_rs;
ID3D11DepthStencilState *g_ds;
ID3D11BlendState *g_bs;
ID3D11ShaderResourceView *g_iconSrv;  // c4icons1: chest icons; null = the old drawn shapes
ID3D11SamplerState *g_smp;
IDXGISwapChain *g_sc;
ID3D11Texture2D *g_bb;
ID3D11RenderTargetView *g_rtv;
UINT g_w, g_h;
LONG g_drawn;

void Release() {
    if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
    if (g_bb) g_bb->Release(), g_bb = nullptr;
    g_sc = nullptr;
}

bool Setup(IDXGISwapChain *sc) {
    if (g_failed) return false;
    if (!g_dev) {
        ID3D11Device *dev = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev))) return g_failed = true, false;
        ID3D11Device1 *dev1 = nullptr;
        if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1))) {
            Log("laser: no D3D11.1 device; aim beam off");
            dev->Release();
            return g_failed = true, false;
        }
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        CompileFn compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        // numstyle1: the pixel shader with the number styles first; if that fails, the plain one (the log says why)
        static const D3D_SHADER_MACRO kStyles[] = {{"NUMSTYLES", "1"}, {nullptr, nullptr}};
        bool psOk = compile && SUCCEEDED(compile(kShader, sizeof kShader - 1, "laser", kStyles, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err));
        if (compile && !psOk) {
            Log("laser: number styles left out, shader said: %.300s", err ? (const char *)err->GetBufferPointer() : "?");
            if (err) err->Release(), err = nullptr;
            psOk = SUCCEEDED(compile(kShader, sizeof kShader - 1, "laser", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err));
        }
        if (!compile || FAILED(compile(kShader, sizeof kShader - 1, "laser", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) || !psOk) {
            Log("laser: shader compile failed%s; aim beam off", compile ? "" : " (no d3dcompiler_47)");
            if (err) err->Release();
            dev1->Release();
            dev->Release();
            return g_failed = true, false;
        }
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
        vsb->Release(), psb->Release();
        D3D11_BUFFER_DESC bd = {sizeof(Cb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        dev->CreateBuffer(&bd, nullptr, &g_cb);
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &g_rs);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dev->CreateDepthStencilState(&dd, &g_ds);
        D3D11_BLEND_DESC bld = {};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA, bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE, bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bld, &g_bs);
        dev->GetImmediateContext(&g_ctx);  // kept
        dev1->Release();
        g_dev = dev;
        if (!g_ctx || !g_vs || !g_ps || !g_cb || !g_rs || !g_ds || !g_bs) {
            Log("laser: setup incomplete; aim beam off");
            return g_failed = true, false;
        }
        {   // c4icons1: the chest icon texture (built in, 1024x256 + mips, ~1.4 MB once)
#include "icons_rgba.inc"
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = 1536, td.Height = 256  /* slowmo2: 6 tiles */, td.MipLevels = kIconMips, td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd[12] = {};
            const unsigned char *p = kIconData;
            UINT w = 1536, h = 256;  // slowmo2
            for (UINT l = 0; l < kIconMips && l < 12; ++l) {
                sd[l].pSysMem = p, sd[l].SysMemPitch = w * 4;
                p += w * h * 4;
                w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1;
            }
            ID3D11Texture2D *tex = nullptr;
            D3D11_SAMPLER_DESC smd = {};
            smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR, smd.MaxLOD = D3D11_FLOAT32_MAX;
            smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            if (p == kIconData + sizeof kIconData && SUCCEEDED(dev->CreateTexture2D(&td, sd, &tex)) &&
                SUCCEEDED(dev->CreateSamplerState(&smd, &g_smp)))
                dev->CreateShaderResourceView(tex, nullptr, &g_iconSrv);
            if (tex) tex->Release();
            if (!g_iconSrv) Log("laser: chest icon texture not made; drawn shapes instead");
        }
        Log("laser: aim beam ready");
    }
    if (sc != g_sc) {
        Release();
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&g_bb))) return false;
        D3D11_TEXTURE2D_DESC td;
        g_bb->GetDesc(&td);
        g_w = td.Width, g_h = td.Height;
        if (FAILED(g_dev->CreateRenderTargetView(g_bb, nullptr, &g_rtv))) {
            Release();
            return false;
        }
        g_sc = sc;
    }
    return true;
}

float Dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

namespace {
void OcclDrop();
}
void LaserOnSwapChainGoing() { OcclDrop(), Release(); }

static volatile bool g_on = true;
// beam1: a laser-pointer look, set from the menu (Aim page): how far the beam
// reaches and how see-through it is; it fades out towards its far end.
static float g_len = 40.0f, g_alpha = 0.75f;
float *LaserLength() { return &g_len; }
// beam2: where the beam starts, metres out from the hand (so it can come out of the muzzle).
static float g_start = 0.05f;
float *LaserStart() { return &g_start; }
static float g_fade = 0.0f;  // beam3: fade-in length (m) from the start
float *LaserFade() { return &g_fade; }
static float g_width = 1.0f;  // beam4: width scale (1 = 4 px near, 1.5 px far)
float *LaserWidth() { return &g_width; }
float *LaserOpacity() { return &g_alpha; }
bool LaserOn() { return g_on; }
// dot1 (DrGonzaga 3 Nov): a soft glow where the beam meets a surface (needs walls on).
static bool g_dotOn = true;
static float g_dotSize = 0.015f, g_dotGlow = 0.6f;  // core radius (m), halo 0-1
static float g_dotRange = 150.0f;  // dot3: the glow shows out to here, beyond the beam (m)
float *LaserDotRange() { return &g_dotRange; }
bool *LaserDotOn() { return &g_dotOn; }
float *LaserDotSize() { return &g_dotSize; }
float *LaserDotGlow() { return &g_dotGlow; }
static int g_colour;  // laser2: 0 ruby red, 1 green, 2 blue, 3 orange (per gun)
int *LaserColour() { return &g_colour; }
void LaserSetOn(bool on) { g_on = on; }

namespace {

// Draw quads (corner clip positions + per-vertex alpha, colour) into this eye's image.
// Save just what we change and put it back after (laser1 used a whole
// device context state swap, which stopped our context hooks seeing the
// game's constant buffer writes: the eyes' poses went wrong, 2D/doubling).
// (ported from the Far Cry 3 mod, 15 Nov: its hand-over "Laser beam and landing dot hidden behind scene geometry")
// ---- occl2 (DrGonzaga 12-13 Nov): the beam goes behind things in front of it ----
// occl1 depth-tested the beam against the depth buffer the game binds with the final
// picture; the log showed the test ran on every eye, but nothing was hidden: that
// buffer does not hold the scene (the scene's depth is the 2x anti-aliased one, which
// cannot be bound with the final picture). occl2: trace.cpp shows us every depth
// buffer the game binds while an eye is drawn; the full-size one bound most often is
// the scene's. At the beam's draw its texture is read by the beam's own pixel shader
// (one view made and released within the draw), each of its samples compared with the
// beam's depth, so edges are smooth. Nothing is kept past the eye's Present, nothing is
// read back to the CPU. Anything missing (scope, menus, shader not compiled, unknown
// format): the beam is drawn on top, as before.
// TRIAL, off by default: [aim] BeamBehindObjects, Beam & scope page.
bool g_occl = true;         // the tickbox (Blood Dragon: on unless unticked)
bool g_occPsFailed;
ID3D11PixelShader *g_occPs;  // the beam with the depth compare (its own shader: a failure leaves the rest alone)
struct OccCand {
    ID3D11DepthStencilView *dsv;  // one reference held until this eye's Present
    ID3D11Texture2D *tex;         // one reference held, the same
    UINT samples;
    int binds;
} g_occCand[6];
int g_occN;
const void *g_occNo[4];  // the last views turned down (shadow maps and the like), to skip the look-up next time
int g_occNoAt;
LONG g_occWith, g_occWithout;  // beam draws with / without the test
DWORD g_occLogTick;
char g_occLast[200];  // what the last eye's choice was, for the log
void OcclDrop() {
    for (int i = 0; i < g_occN; ++i) g_occCand[i].dsv->Release(), g_occCand[i].tex->Release();
    g_occN = 0, g_occNo[0] = g_occNo[1] = g_occNo[2] = g_occNo[3] = nullptr;
}
const char kOccPs[] =
    "cbuffer C : register(b0) { float4 corner[4]; float4 color; float4 win[2]; float4 dz; float4 occ; };\n"
    "struct V { float4 p : SV_Position; float a : A; float2 uv : U; };\n"
    "Texture2D<float> d1 : register(t1);\n"
    "Texture2DMS<float> dms : register(t2);\n"
    "float seen(int2 p, float z) {\n"
    "  if (occ.x < 1.5) return z <= d1.Load(int3(p, 0)) ? 1 : 0;\n"
    "  int n = min((int)occ.x, 8);\n"
    "  float v = 0;\n"
    "  for (int s = 0; s < n; ++s) v += z <= dms.Load(p, s) ? 1 : 0;\n"
    "  return v / n; }\n"
    "float4 ps(V i) : SV_Target {\n"
    "  float z = i.p.z - 0.00002;\n"
    "  float2 g = float2(ddx(i.uv.x), ddy(i.uv.y));\n"  // (taken before any branch)
    "  if (corner[0].w < 1.5) {\n"  // the beam: each pixel on its own
    "    float vis = seen(int2(i.p.xy), z);\n"
    "    if (vis <= 0) discard;\n"
    "    return float4(color.rgb, color.a * i.a * vis); }\n"
    // occl3: the dot where the beam lands is hidden or shown as a whole, by its centre
    // (five points around it, so it fades over an edge instead of popping). The centre's
    // pixel comes from the quad's own coordinates (uv is 0 there).
    "  float2 c = i.p.xy - float2(i.uv.x / g.x, i.uv.y / g.y);\n"
    "  float vis = (seen(int2(c), z) + seen(int2(c + float2(1.5, 1.5)), z) + seen(int2(c + float2(-1.5, 1.5)), z)\n"
    "    + seen(int2(c + float2(1.5, -1.5)), z) + seen(int2(c + float2(-1.5, -1.5)), z)) * 0.2;\n"
    "  if (vis <= 0) discard;\n"
    // (the same look as the main shader's dot)
    "  float r = length(i.uv), cr = corner[2].w;\n"
    "  float core = 1 - smoothstep(cr * 0.15, cr * 1.25, r);\n"
    "  float halo = saturate((exp(-r * r * 5) - 0.0067) / 0.9933) * corner[1].w;\n"
    "  float k = max(color.a, 1), al = min(color.a, 1);\n"
    "  return float4(saturate(lerp(color.rgb * k, 1, core * core * 0.3 * k)), al * saturate(max(core, halo * k)) * vis); }\n";
// The scene's depth among this eye's candidates: the one bound most often (more samples wins a tie).
const OccCand *OcclPick() {
    const OccCand *b = nullptr;
    for (int i = 0; i < g_occN; ++i)
        if (!b || g_occCand[i].binds > b->binds || (g_occCand[i].binds == b->binds && g_occCand[i].samples > b->samples)) b = &g_occCand[i];
    return b;
}

// occl2 / occl3: quads marked occ[0] = 1 (the beam, the dot where it lands) are compared
// with the scene's depth buffer when this eye has one; the rest go on top as always.
void DrawQuads(const Cb *quads, int n, int nDepth = 0) {
    ID3D11DeviceContext *c = g_ctx;
    HeadtrackOwnDraw(true);
    D3DState game(c, 1, 1);  // state1: the game's pipeline state, put back by Restore() below (c4icons1: + texture 0, sampler 0)

    D3D11_VIEWPORT vp = {0, 0, (float)g_w, (float)g_h, 0, 1};
    EngineEyeViewport(&vp.TopLeftX, &vp.TopLeftY, &vp.Width, &vp.Height);  // oc1
    c->RSSetViewports(1, &vp);
    c->RSSetState(g_rs);
    c->OMSetDepthStencilState(g_ds, 0);
    float bf[4] = {0, 0, 0, 0};
    c->OMSetBlendState(g_bs, bf, 0xffffffff);
    c->OMSetRenderTargets(1, &g_rtv, nullptr);  // (also takes the game's depth buffer off the pipeline, so a shader may read it)
    // occl2: the scene's depth for the beam's shader, a view made here and let go below.
    ID3D11ShaderResourceView *occSrv = nullptr, *was[2] = {nullptr, nullptr};
    const OccCand *oc = nDepth > 0 && g_occl ? OcclPick() : nullptr;
    if (oc && !g_occPs && !g_occPsFailed) {
        HMODULE dc = GetModuleHandleW(L"d3dcompiler_47.dll");
        CompileFn compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        ID3DBlob *psb = nullptr, *err = nullptr;
        if (compile && SUCCEEDED(compile(kOccPs, sizeof kOccPs - 1, "laserocc", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err)))
            g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_occPs);
        if (!g_occPs) {
            g_occPsFailed = true;
            Log("laser: behind objects: shader not made (%.300s); the beam stays on top", err ? (const char *)err->GetBufferPointer() : "no compiler");
        }
        if (psb) psb->Release();
        if (err) err->Release();
    }
    if (oc && g_occPs) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        if (oc->samples > 1) sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        else sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sd.Texture2D.MipLevels = 1;
        HRESULT hr = g_dev->CreateShaderResourceView(oc->tex, &sd, &occSrv);
        if (FAILED(hr)) occSrv = nullptr;
        snprintf(g_occLast, sizeof g_occLast, "%u sample(s), bound %d times, view %s", oc->samples, oc->binds, occSrv ? "made" : "REFUSED");
    } else if (nDepth > 0 && g_occl)
        snprintf(g_occLast, sizeof g_occLast, "%s", oc ? "no shader" : "no full-size depth buffer seen this eye");
    const bool test = occSrv != nullptr;
    if (nDepth > 0 && g_occl) ++(test ? g_occWith : g_occWithout);
    if (!test) nDepth = 0;
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(g_vs, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &g_cb);
    c->PSSetShader(g_ps, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &g_cb);
    if (g_iconSrv) c->PSSetShaderResources(0, 1, &g_iconSrv), c->PSSetSamplers(0, 1, &g_smp);
    if (test) {
        c->PSGetShaderResources(1, 2, was);
        ID3D11ShaderResourceView *set[2] = {oc->samples > 1 ? nullptr : occSrv, oc->samples > 1 ? occSrv : nullptr};
        c->PSSetShaderResources(1, 2, set);
    }
    for (int i = 0; i < n; ++i) {
        const bool tq = test && quads[i].occ[0] > 0.5f;
        if (test && (i == 0 || tq != (quads[i - 1].occ[0] > 0.5f))) c->PSSetShader(tq ? g_occPs : g_ps, nullptr, 0);
        if (tq) {
            Cb q = quads[i];
            q.occ[0] = (float)oc->samples;
            c->UpdateSubresource(g_cb, 0, nullptr, &q, 0, 0);
        } else
            c->UpdateSubresource(g_cb, 0, nullptr, &quads[i], 0, 0);
        c->Draw(4, 0);
    }
    if (test) {  // the depth texture is off the pipeline before the game's state goes back
        c->PSSetShaderResources(1, 2, was);
        if (was[0]) was[0]->Release();
        if (was[1]) was[1]->Release();
        occSrv->Release();
    }

    game.Restore();
    HeadtrackOwnDraw(false);  // after putting the game's targets back (hud.cpp counts the game's own binds)
}

// A quad between two pixel points with half-widths w0, w1 and alphas a0, a1.
void Segment(Cb &q, float sx0, float sy0, float sx1, float sy1, float w0, float w1, float a0, float a1,
                    const float rgba[4]) {
    float dx = sx1 - sx0, dy = sy1 - sy0, l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-3f) dx = 1, dy = 0, l = 1;
    float nx = -dy / l, ny = dx / l;
    float px[4] = {sx0 + nx * w0, sx0 - nx * w0, sx1 + nx * w1, sx1 - nx * w1};
    float py[4] = {sy0 + ny * w0, sy0 - ny * w0, sy1 + ny * w1, sy1 - ny * w1};
    q = Cb{};
    for (int k = 0; k < 4; ++k) {
        q.corner[k][0] = px[k] / g_w * 2 - 1;
        q.corner[k][1] = 1 - py[k] / g_h * 2;
        q.corner[k][2] = k < 2 ? a0 : a1;
        q.corner[k][3] = 1;
    }
    memcpy(q.color, rgba, 16);
}

// Aim offset (Aim page), degrees right / up about the controller's own axes.
float g_xOff[2];

}  // namespace

float *LaserOffset() { return g_xOff; }

// The hand and its aim in the game world, as the view draws them: the beam
// starts at hand and runs along dir (with the Aim page offset); raw is the
// controller's own forward, without the offset (the shots' look bone, bonehook).
bool LaserRay(EngineView &v, float hand[3], float dir[3], float raw[3]) {
    float m[16], off[3];
    if (!EngineLastView(&v) || !HeadtrackHand(1, m, off)) return false;
    const float *r = v.basis, *u = v.basis + 3, *f = v.basis + 6;
    const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
    for (int i = 0; i < 3; ++i) {
        float o[3] = {off[0] / v.scale, off[1] / v.scale, off[2] / v.scale};
        hand[i] = v.base[i] + (v.sixdof ? fr[i] * o[0] + wu[i] * o[1] - ff[i] * o[2] : 0);
        dir[i] = raw[i] = -(r[i] * m[0 * 4 + 2] + u[i] * m[1 * 4 + 2] - f[i] * m[2 * 4 + 2]);
    }
    // Offset (Aim page), degrees right / up about the controller's own axes.
    if (g_xOff[0] != 0 || g_xOff[1] != 0) {
        float tr = tanf(g_xOff[0] * 0.0174533f), tu = tanf(g_xOff[1] * 0.0174533f), l = 0;
        for (int i = 0; i < 3; ++i) {
            float hr = r[i] * m[0] + u[i] * m[4] - f[i] * m[8], hu = r[i] * m[1] + u[i] * m[5] - f[i] * m[9];
            dir[i] += tr * hr + tu * hu;
            l += dir[i] * dir[i];
        }
        l = sqrtf(l);
        for (int i = 0; i < 3; ++i) dir[i] /= l;
    }
    return true;
}
bool LaserRay(float hand[3], float dir[3], float raw[3]) {
    EngineView v;
    return LaserRay(v, hand, dir, raw);
}

// ---- Beam stops at walls (wall1, 2 Nov; ray1, 3 Nov) ----
// The first surface along the beam comes from the game's own collision ray
// (LaserGameTick, game thread, below). The old GPU depth march (wall1-3, dot1-3)
// held the game's depth buffers and made views of them; with it off, the
// render-target crash (FC3+114f2eab) stopped, so it was removed (DrGonzaga 3 Nov).
static volatile bool g_wallOn = true;
bool *LaserStopsAtWalls() { return (bool *)&g_wallOn; }
// dotonly1 (DrGonzaga 8 Nov): just the red dot where the beam would hit, no beam.
// Needs the game's collision ray, so it keeps that running even with "Stops at walls" off.
static volatile bool g_dotOnly = false;  // [aim] DotOnly
bool *LaserDotOnly() { return (bool *)&g_dotOnly; }
bool *LaserBehindObjects() { return &g_occl; }
// trace.cpp (render thread, the game's immediate context): the game binds this depth view.
void LaserNoteGameDepth(ID3D11DepthStencilView *dsv) {
    if (!g_occl || !g_bb || HeadtrackOwn() || dsv == g_occNo[0] || dsv == g_occNo[1] || dsv == g_occNo[2] || dsv == g_occNo[3]) return;
    for (int i = 0; i < g_occN; ++i)
        if (g_occCand[i].dsv == dsv) {
            ++g_occCand[i].binds;
            return;
        }
    ID3D11Resource *r = nullptr;
    ID3D11Texture2D *t = nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    dsv->GetResource(&r);
    bool ok = r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t));
    if (ok) t->GetDesc(&d);
    if (r) r->Release();
    // The picture's size, readable by a shader, the 24-bit depth format the game uses, not an array.
    ok = ok && g_occN < 6 && d.Width == g_w && d.Height == g_h && d.ArraySize == 1 && d.Format == DXGI_FORMAT_R24G8_TYPELESS &&
         (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && d.SampleDesc.Count >= 1 && d.SampleDesc.Count <= 8;
    if (!ok) {
        if (t) t->Release();
        g_occNo[g_occNoAt++ & 3] = dsv;
        return;
    }
    dsv->AddRef();
    g_occCand[g_occN++] = {dsv, t, d.SampleDesc.Count, 1};  // t's reference is kept
}
// hook.cpp, every Present after the overlays: this eye's depth views are let go.
void LaserEyeDone() {
    if (!g_occl) {
        if (g_occN) OcclDrop();
        return;
    }
    DWORD now = GetTickCount();
    if (now - g_occLogTick > 30000 && (g_occWith || g_occWithout) && g_verboseLog) {  // tidy1 (bd79): detailed log only
        float a = 0, b = 0;
        bool have = EngineDepthTerms(&a, &b);
        char c[160] = "";
        int k = 0;
        for (int i = 0; i < g_occN; ++i)
            k += snprintf(c + k, sizeof c - k, " [%u sample(s), bound %d times]", g_occCand[i].samples, g_occCand[i].binds);
        Log("laser: behind objects: last 30 s: %ld beam draws tested, %ld on top; depth = %.5f + %.5f / distance%s; this eye's full-size depth buffers:%s; last choice: %s",
            g_occWith, g_occWithout, a, b, have ? "" : " (projection not read)", g_occN ? c : " none", g_occLast);
        g_occWith = g_occWithout = 0, g_occLogTick = now;
    }
    OcclDrop();
}

// lasers1 (DrGonzaga 10 Nov): mounted guns show just a bright dot by default, out to 250 m.
static bool g_turDotOnly = true;      // [vr] TurretDotOnly
static float g_turDotRange = 250.0f;  // [vr] TurretDotRange (m)
bool *LaserTurretDotOnly() { return &g_turDotOnly; }
float *LaserTurretDotRange() { return &g_turDotRange; }
// lasers1 (DrGonzaga 10 Nov): while a grenade / molotov is being thrown, a short fat stub
// from the hand shows the throw's angle, with no dot and no wall ray.
static bool g_thrOn = true;                                  // [aim] ThrowBeam
static float g_thrLen = 0.35f, g_thrWidth = 5.0f, g_thrAlpha = 0.8f;  // ThrowBeamLength (m), ThrowBeamWidth, ThrowBeamOpacity
static int g_thrColour = 0;  // lasers2: [aim] ThrowBeamColour, its own (it flashed the last gun's colour first)
int *LaserThrowColour() { return &g_thrColour; }
// lasers2 (DrGonzaga 10 Nov): no laser at all for this gun ([gun.<name>] BeamOff; mines default off).
static volatile bool g_gunOff;
static float g_dotBright = 1.0f;  // lasers3: dot brightness (per gun), 0.05..1
float *LaserDotBright() { return &g_dotBright; }
bool *LaserGunOff() { return (bool *)&g_gunOff; }
// sight1 (DrGonzaga 10 Nov): reflex-sight dot. Per gun, the dot shows only where the line
// from an eye to it passes through a window fixed to the gun (the sight's glass), like a
// real red dot sight; no beam. Window in the controller frame (metres): right, up, forward;
// width/height (circle: width = diameter); fade from this fraction of the edge outwards.
static volatile bool g_sightOn;                  // [gun.x] SightDot
static int g_sightShape;                         // [gun.x] SightShape 0 circle, 1 rectangle
static float g_sight[12] = {0, 0.07f, 0.10f, 0.035f, 0.025f, 0.7f,  // SightRight, SightUp, SightForward, SightWidth, SightHeight, SightFade
                            0, 1, 0.6f, 3, 1, 0};  // + sight4: SightReticleTurn (deg, clockwise)  // sight3: SightRoll (deg), SightBrightness (0.05-2), SightSoftness (0-2), SightThickness (MOA), SightSize (x)
static int g_sightRet;                           // sight3: [sight.x] SightReticle 0 dot, 1 holo ring, 2 chevron, 3 cross, 4 triangle, 5 bow pins (sight6)
static float g_sightMoa = 3.0f;                  // [aim] SightDotMoa: dot size, minutes of angle
static bool g_sightOutline;                      // menu only, not saved: show the window's edge
bool *LaserSightOn() { return (bool *)&g_sightOn; }
// sight5 (DrGonzaga 10 Nov): beam and red dot where it lands, each on or off per gun and optic
// ([sight.<gun>.<optic>] ShowBeam, ShowHitDot; reticle = SightDot). Defaults from the older per-gun ticks.
static volatile bool g_showBeam = true, g_showDot = true;
bool *LaserShowBeam() { return (bool *)&g_showBeam; }
bool *LaserShowDot() { return (bool *)&g_showDot; }
// occl4 (DrGonzaga 15 Nov: "the dot gets hidden behind grass, I want the laser to occlude but the dot to remain visible"):
// beam and dot each go behind things or not, per gun and optic ([sight.<gun>.<optic>] BeamBehind 1, DotBehind 0).
static volatile bool g_beamBehind = true, g_dotBehind = false;
bool *LaserBeamBehind() { return (bool *)&g_beamBehind; }
bool *LaserDotBehind() { return (bool *)&g_dotBehind; }
int *LaserSightShape() { return &g_sightShape; }
float *LaserSightValue(int i) { return i >= 0 && i < 12 ? &g_sight[i] : &g_sight[0]; }
int *LaserSightReticle() { return &g_sightRet; }
float *LaserSightMoa() { return &g_sightMoa; }
bool *LaserSightOutline() { return &g_sightOutline; }
bool *LaserThrowOn() { return &g_thrOn; }
float *LaserThrowValue(int i) {
    float *p[3] = {&g_thrLen, &g_thrWidth, &g_thrAlpha};
    return i >= 0 && i < 3 ? p[i] : &g_thrLen;
}
static SRWLOCK g_rayLock = SRWLOCK_INIT;
static struct {
    float hand[3], dir[3], cam[3];
    DWORD tick;
} g_ray;  // the left eye's beam (before the recoil), for the game thread
static volatile float g_wDist = -1;  // latest hit distance along the beam (world units), -1 none
static volatile DWORD g_wTick;       // when it was found

// nogun1 (DrGonzaga 6 Nov): no beam while there's no weapon in the hand.
static bool g_noGunOff = true;  // [aim] BeamOffWithoutGun
bool *LaserOffWithoutGun() { return &g_noGunOff; }
// turret1 (DrGonzaga 9 Nov): on a mounted gun, a beam from just in front of and below
// the eyes to where the game's camera aims (the gun's crosshair), meeting it at the
// last hit distance, so it sits on what the gun will hit.
static volatile bool g_turBeam;
static bool TurretRay(EngineView &v, float hand[3], float dir[3]) {
    float cp[3], cf[3];
    if (!EngineLastView(&v) || !EngineTurretAim(cp, cf)) return false;
    const float *ff = v.posAxes + 6;  // level forward
    for (int i = 0; i < 3; ++i) hand[i] = v.base[i] + ff[i] * 0.45f - (i == 2 ? 0.30f : 0.0f);
    float d = g_wDist > 0 && GetTickCount() - g_wTick < 250 ? g_wDist + 0.45f : 60.0f;
    const float dmax = g_turDotRange > 150 ? g_turDotRange : 150;  // lasers1: out to 250 m
    d = d < 5 ? 5 : d > dmax ? dmax : d;
    float l = 0;
    for (int i = 0; i < 3; ++i) dir[i] = cp[i] + cf[i] * d - hand[i], l += dir[i] * dir[i];
    l = sqrtf(l);
    if (l < 1e-3f) return false;
    for (int i = 0; i < 3; ++i) dir[i] /= l;
    return true;
}

void LaserDraw(IDXGISwapChain *sc) {
    bool tur = g_turretLaser && EngineTurret();
    g_turBeam = tur;
    if (!tur && (!g_on || !BoneHookAiming() || !EngineOnFoot() || CyberEyeOn())) return;
    if (!tur && GunAttachNoLaser()) return;  // nolaser1 (DrGonzaga 16 Nov): none while injecting or holding the cyber heart
    // dice1 (DrGonzaga 17 Nov: "remove the laser beam from the D20 dice throw ... a very short (20cm or so) faded laser,
    // with no red dot landing, to communicate the direction of throw"): while the right stick is held up (the dice
    // throw) or the dice ("Rock") is in the hand, a 20 cm faded stub as for a throw, no dot.
    const bool dice = !tur && g_bloodDragon && (InputStickUpHeld() || !strcmp(GunAttachName(), "Rock"));
    const bool thr = !tur && (dice || (g_thrOn && InputThrowAiming()));  // lasers1: throw stub (the gun may be put away)
    if (!tur && !thr && g_noGunOff && GetTickCount() - GunAttachHandTick() > 400) return;
    if (!tur && !thr && !g_showBeam && !g_showDot && !g_sightOn) return;  // lasers2 / sight5: nothing for this gun and optic
    EngineView v;
    float tx, ty, hand[3], dir[3], raw[3];
    if (!(tur ? TurretRay(v, hand, dir) : LaserRay(v, hand, dir, raw)) || !XrGetHeadsetFov(&tx, &ty)) return;
    if (!Setup(sc)) return;
    // On the turret: a long beam always (even with "dot only"), starting at its origin.
    // lasers1: on the turret, just the dot unless 'dot only' is unticked there; throwing, a short fat stub.
    const float len = tur ? 80.0f : dice ? 0.20f : thr ? g_thrLen : g_len, start = tur ? 0.0f : thr ? 0.05f : g_start,
                fade = tur ? 0.5f : thr ? 0.0f : g_fade;
    const float width = thr ? g_thrWidth : g_width, beamAlpha = dice ? g_thrAlpha * 0.5f : thr ? g_thrAlpha : g_alpha;
    const bool sight = !tur && !thr && g_sightOn;  // sight1
    const bool dotOnly = tur ? g_turDotOnly : thr ? false : !g_showBeam;  // sight5: no beam
    const bool walls = !thr && (g_wallOn || dotOnly || tur || g_showDot || sight);
    float F[9], R[9];  // sight1: the controller frame (right, forward, up) and the kick's turn, for the window
    bool haveF = false, kicked = false;
    if (!tur && !thr) {
        // kick1: the beam kicks with the gun (same turn about the hand, same slide back).
        float m[16], bk[3];
        if (HeadtrackHand(1, m, nullptr)) {
            haveF = true;
            const float *r = v.basis, *u = v.basis + 3, *f = v.basis + 6;
            for (int i = 0; i < 3; ++i) {
                F[i] = r[i] * m[0] + u[i] * m[4] - f[i] * m[8];
                F[3 + i] = -(r[i] * m[2] + u[i] * m[6] - f[i] * m[10]);
                F[6 + i] = r[i] * m[1] + u[i] * m[5] - f[i] * m[9];
            }
            if (GunKickNow(F, R, bk)) {
                float o[3];
                kicked = true;
                // aimdev2 (DrGonzaga 9 Nov): the beam turns with the gun again (same turn
                // about the same point), so it stays lined up with the barrel; with
                // aimdev1's "follow" on, that turn is towards where each bullet went.
                GunKickRotate(R, dir, o), memcpy(dir, o, 12);
                for (int i = 0; i < 3; ++i) hand[i] += bk[i];
            }
        }
    }
    if (walls) {
        // ray1: hand the left eye's beam (aimdev1: after the turn to the bullets, so the dot marks where they hit) to the game thread's collision ray.
        int eye = -1;
        float q[4], pp[3];
        if (!EngineLastEye(&eye, q, pp)) eye = -1;
        if (eye <= 0) {
            AcquireSRWLockExclusive(&g_rayLock);
            memcpy(g_ray.hand, hand, 12), memcpy(g_ray.dir, dir, 12), memcpy(g_ray.cam, v.pos, 12), g_ray.tick = GetTickCount();
            ReleaseSRWLockExclusive(&g_rayLock);
        }
    }
    // Into this eye's view: right = forward x up, as the stereo offset uses.
    float nr[3] = {v.fwd[1] * v.up[2] - v.fwd[2] * v.up[1], v.fwd[2] * v.up[0] - v.fwd[0] * v.up[2],
                   v.fwd[0] * v.up[1] - v.fwd[1] * v.up[0]};
    Cb quads[40];
    int nq = 0, nBeam = 0;
    float occA = 0, occB = 0;
    const bool occ = g_occl && g_occN && EngineDepthTerms(&occA, &occB);  // occl2
    // beam3: ruby red (laser2: or green / blue / orange); fades in over g_fade from where it starts, then out
    // towards its far end. Width runs from 4 px at the start to 1.5 px at the end.
    // wall3 (DrGonzaga 2 Nov): the beam's look (fade-in, fade-out, width) stays
    // laid out over its full length; a wall just cuts it off at tCut.
    const float tEnd = len > start + 0.05f ? len : start + 0.05f;
    const float tFade = start + (fade < tEnd - start ? fade : tEnd - start);
    float tCut = tEnd;
    if (!thr && (g_wallOn || tur) && g_wDist > 0 && GetTickCount() - g_wTick < 250 && g_wDist < tCut) tCut = g_wDist;
    auto piece = [&](float t0, float t1, float a0, float a1) {
        if (t1 <= t0) return;
        float w0 = width * (4.0f + (1.5f - 4.0f) * (t0 - start) / (tEnd - start));
        float w1 = width * (4.0f + (1.5f - 4.0f) * (t1 - start) / (tEnd - start));
        float p0[3], p1[3];
        for (int i = 0; i < 3; ++i) {
            p0[i] = hand[i] + dir[i] * t0 - v.pos[i];
            p1[i] = hand[i] + dir[i] * t1 - v.pos[i];
        }
        float z0 = Dot(p0, v.fwd), z1 = Dot(p1, v.fwd);
        const float kNear = 0.05f;
        if (z0 < kNear && z1 < kNear) return;
        if (z0 < kNear || z1 < kNear) {  // clip to the near plane
            float t = (kNear - z0) / (z1 - z0);
            float c[3] = {p0[0] + (p1[0] - p0[0]) * t, p0[1] + (p1[1] - p0[1]) * t, p0[2] + (p1[2] - p0[2]) * t};
            float ac = a0 + (a1 - a0) * t, wc = w0 + (w1 - w0) * t;
            if (z0 < kNear) memcpy(p0, c, 12), z0 = kNear, a0 = ac, w0 = wc;
            else memcpy(p1, c, 12), z1 = kNear, a1 = ac, w1 = wc;
        }
        float sx0 = (Dot(p0, nr) / (z0 * tx) * 0.5f + 0.5f) * g_w, sy0 = (0.5f - Dot(p0, v.up) / (z0 * ty) * 0.5f) * g_h;
        float sx1 = (Dot(p1, nr) / (z1 * tx) * 0.5f + 0.5f) * g_w, sy1 = (0.5f - Dot(p1, v.up) / (z1 * ty) * 0.5f) * g_h;
        static const float kCol[4][3] = {{0.80f, 0.04f, 0.20f},   // ruby / crimson
                                         {0.10f, 0.90f, 0.20f},   // green
                                         {0.15f, 0.45f, 1.00f},   // blue
                                         {1.00f, 0.45f, 0.05f}};  // orange
        const float *k = kCol[(thr ? g_thrColour : g_colour) & 3];
        const float col[4] = {k[0], k[1], k[2], beamAlpha};
        Segment(quads[nq++], sx0, sy0, sx1, sy1, w0, w1, a0, a1, col);
        if (occ && !thr && (tur || g_beamBehind)) {  // occl1: where each end sits in the game's depth buffer
            Cb &q = quads[nq - 1];
            float d0 = occA + occB / z0, d1 = occA + occB / z1;
            d0 = d0 < 0 ? 0 : d0 > 1 ? 1 : d0, d1 = d1 < 0 ? 0 : d1 > 1 ? 1 : d1;
            q.dz[0] = q.dz[1] = d0, q.dz[2] = q.dz[3] = d1;
            q.occ[0] = 1, ++nBeam;
        }
    };
    // Brightness along the beam after the fade-in: 1 at tFade down to 0.1 at the end.
    auto alpha = [&](float t) {  // brightness at t on the full beam
        if (t < tFade) return tFade > start ? (t - start) / (tFade - start) : 1.0f;
        return tEnd > tFade ? 1.0f + (0.1f - 1.0f) * (t - tFade) / (tEnd - tFade) : 0.1f;
    };
    if (tCut > start && !dotOnly) {
        float a = tFade < tCut ? tFade : tCut;
        piece(start, a, 0, alpha(a));
        if (tCut > tFade) piece(tFade, tCut, 1, alpha(tCut));
    }
    // dot3: the glow follows any hit within its own range, past the end of the beam too.
    float tHit = walls && g_wDist > 0 && GetTickCount() - g_wTick < 250 ? g_wDist : -1;
    const float tReal = tHit;  // sight5: the real hit (the sight's dot falls back to far out)
    const float dotReach = tur ? g_turDotRange : g_dotRange > tEnd ? g_dotRange : tEnd;
    // sight1: the window on the gun (centre C, axes ax[0] right, ax[1] up, ax[2] forward), turned with the kick.
    float C[3], ax[3][3], sightA = 1;
    bool sightIn = false;
    const bool win = sight && haveF;
    if (win) {
        const float *a[3] = {F, F + 6, F + 3};
        for (int j = 0; j < 3; ++j) {
            if (kicked) GunKickRotate(R, a[j], ax[j]);
            else memcpy(ax[j], a[j], 12);
        }
        for (int i = 0; i < 3; ++i) C[i] = hand[i] + ax[0][i] * g_sight[0] + ax[1][i] * g_sight[1] + ax[2][i] * g_sight[2];
        if (g_sight[6] != 0) {  // sight3: the window (and its reticle) turned about the gun's forward, clockwise
            float c = cosf(g_sight[6] * 0.0174533f), sn = sinf(g_sight[6] * 0.0174533f), r2[3], u2[3];
            for (int i = 0; i < 3; ++i) r2[i] = ax[0][i] * c - ax[1][i] * sn, u2[i] = ax[1][i] * c + ax[0][i] * sn;
            memcpy(ax[0], r2, 12), memcpy(ax[1], u2, 12);
        }
        if (tHit <= 0 || tHit > dotReach) tHit = 300.0f;  // nothing hit: the dot still shows, far out (like a real sight)
        // Does the line from this eye to the dot pass through the window? Where, as a fraction of its edge?
        float P[3], e2p[3], e2c[3];
        for (int i = 0; i < 3; ++i) P[i] = hand[i] + dir[i] * tHit, e2p[i] = P[i] - v.pos[i], e2c[i] = C[i] - v.pos[i];
        float dn = Dot(e2p, ax[2]), cn = Dot(e2c, ax[2]);
        sightA = 0;
        sightIn = dn > 1e-4f && cn > 0.01f;  // sight4: the window is in front of this eye (reticles fade per pixel)
        if (dn > 1e-4f && cn > 0.01f) {  // window in front of the eye, dot beyond it
            float t = cn / dn, x = 0, y = 0;
            for (int i = 0; i < 3; ++i) {
                float d = v.pos[i] + e2p[i] * t - C[i];
                x += d * ax[0][i], y += d * ax[1][i];
            }
            const float hw = g_sight[3] * 0.5f > 0.002f ? g_sight[3] * 0.5f : 0.002f,
                        hh = g_sight[4] * 0.5f > 0.002f ? g_sight[4] * 0.5f : 0.002f;
            float r = g_sightShape ? fmaxf(fabsf(x) / hw, fabsf(y) / hh) : sqrtf(x * x + y * y) / hw;
            float f0 = g_sight[5] < 0 ? 0 : g_sight[5] > 0.99f ? 0.99f : g_sight[5];
            sightA = r <= f0 ? 1 : r >= 1 ? 0 : 1 - (r - f0) / (1 - f0);
        }
        if (g_sightOutline) {  // the window's edge, thin cyan, for lining it up with the sight model
            const int nseg = g_sightShape ? 4 : 28;
            const float hw = g_sight[3] * 0.5f, hh = g_sight[4] * 0.5f;
            auto edge = [&](int k, float out[2]) {
                if (g_sightShape) {
                    static const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
                    out[0] = sx[k & 3] * hw, out[1] = sy[k & 3] * hh;
                } else {
                    float a = 6.2831853f * k / nseg;
                    out[0] = cosf(a) * hw, out[1] = sinf(a) * hw;
                }
            };
            const float cyan[4] = {0.2f, 0.9f, 1.0f, 0.9f};
            for (int k = 0; k < nseg && nq < 38; ++k) {
                float e0[2], e1[2], s[2][2];
                edge(k, e0), edge(k + 1, e1);
                bool ok = true;
                for (int j = 0; j < 2 && ok; ++j) {
                    const float *e = j ? e1 : e0;
                    float p[3];
                    for (int i = 0; i < 3; ++i) p[i] = C[i] + ax[0][i] * e[0] + ax[1][i] * e[1] - v.pos[i];
                    float z = Dot(p, v.fwd);
                    if (z < 0.02f) ok = false;
                    else s[j][0] = (Dot(p, nr) / (z * tx) * 0.5f + 0.5f) * g_w, s[j][1] = (0.5f - Dot(p, v.up) / (z * ty) * 0.5f) * g_h;
                }
                if (ok) Segment(quads[nq++], s[0][0], s[0][1], s[1][0], s[1][1], 0.8f, 0.8f, 1, 1, cyan);
            }
        }
    }
    // sight5 (DrGonzaga 10 Nov): the red dot where it lands and the sight's reticle are separate,
    // either, both or neither (per gun and optic), so the quad maker runs once for each.
    auto dotQuad = [&](const bool win, const float tHit) {
        if ((win || tur || (!thr && g_showDot)) && tHit > 0 && tHit <= (win ? 300.0f : dotReach) && (!win || sightA > 0.001f || (g_sightRet >= 1 && g_sightRet <= 9 && sightIn))) {
            // dot1: at the hit, pulled 2 cm back towards the gun, facing this eye; a fixed
            // size in the world, so it shrinks with distance like a real dot would.
            float t = tHit - 0.02f > start ? tHit - 0.02f : start, p[3];
            for (int i = 0; i < 3; ++i) p[i] = hand[i] + dir[i] * t - v.pos[i];
            float z = Dot(p, v.fwd);
            if (z > 0.1f) {
                float sx = (Dot(p, nr) / (z * tx) * 0.5f + 0.5f) * g_w, sy = (0.5f - Dot(p, v.up) / (z * ty) * 0.5f) * g_h;
                const float glowS = win ? g_sight[8] : g_dotGlow;  // sight3: the sight's own softness
                float grow = 1 + 6 * glowS;  // the halo reaches out to 7x the core (dot2: 2x dot1; soft1: up to 13x at 2.0)
                // lasers1: the turret's dot is twice the size and keeps at least 3 px of core, so it reads far out.
                // sight1: a sight's dot keeps one angular size (minutes of angle), whatever it lands on.
                const float ds = win ? g_sight[9] * 0.000290888f * z : tur ? g_dotSize * 2 : g_dotSize, minCore = tur ? 3.0f : 1.5f;
                float rx = ds * grow / (z * tx) * 0.5f * g_w, ry = ds * grow / (z * ty) * 0.5f * g_h;
                if (rx < minCore * grow) rx = ry = minCore * grow;  // keep at least a pixel or two of core
                Cb &q = quads[nq++];
                q = Cb{};
                for (int k = 0; k < 4; ++k) {
                    float px = sx + ((k & 1) ? rx : -rx), py = sy + ((k >> 1) ? -ry : ry);
                    q.corner[k][0] = px / g_w * 2 - 1, q.corner[k][1] = 1 - py / g_h * 2, q.corner[k][2] = 1;
                }
                q.corner[0][3] = 2, q.corner[1][3] = glowS > 1 ? 1 : glowS, q.corner[2][3] = 1 / grow;
                static const float kCol[4][3] = {{0.80f, 0.04f, 0.20f}, {0.10f, 0.90f, 0.20f}, {0.15f, 0.45f, 1.00f}, {1.00f, 0.45f, 0.05f}};
                const float *k = kCol[g_colour & 3];
                q.color[0] = k[0], q.color[1] = k[1], q.color[2] = k[2], q.color[3] = tur ? 1 : g_dotBright < 0.02f ? 0.02f : g_dotBright > 2 ? 2 : g_dotBright;  // lasers3 (sight3: up to 2x)
                if (win) q.color[3] = g_sight[7] < 0.02f ? 0.02f : g_sight[7] > 2 ? 2 : g_sight[7];  // sight3: the sight's own brightness
                if (tur) q.corner[1][3] = g_dotGlow < 0.8f ? 0.8f : g_dotGlow > 1 ? 1 : g_dotGlow;  // lasers1: bright halo
                if (win) q.color[3] *= sightA;  // sight1: fades at the window's edge
                if (!win && occ && (tur || g_dotBehind)) {  // occl4
                    // occl3 (DrGonzaga 13 Nov): the dot is hidden by things in front of it too. Compared 3 cm + 3%
                    // nearer than where it sits, so the surface it lands on (often at a slant) never hides it.
                    const float zd = z * 0.97f - 0.03f > 0.11f ? z * 0.97f - 0.03f : 0.11f;
                    float d = occA + occB / zd;
                    d = d < 0 ? 0 : d > 1 ? 1 : d;
                    q.dz[0] = q.dz[1] = q.dz[2] = q.dz[3] = d, q.occ[0] = 1, ++nBeam;
                }
                if (win && g_sightRet >= 1 && g_sightRet <= 9) {
                    // sight3: a reticle instead of the dot, the same place and colour, a fixed angular size.
                    static const float kExt[10] = {0, 36, 8, 17, 6, 74, 23, 26, 40, 42};  // future1: 6-9  // sight8: bow pins reach the widest ring  // sight7: bow pins reach the wider ring  // sight6: 5 = bow pins  // half-size of each, minutes of angle
                    const float sc = g_sight[10] < 0.25f ? 0.25f : g_sight[10], moa = 0.000290888f;
                    const float ext = kExt[g_sightRet] + 2 + 3 * g_sight[8];  // room for the glow
                    float qx = tanf(ext * sc * moa) / tx * 0.5f * g_w, qy = tanf(ext * sc * moa) / ty * 0.5f * g_h;
                    for (int k = 0; k < 4; ++k) {
                        float px = sx + ((k & 1) ? qx : -qx), py = sy + ((k >> 1) ? -qy : qy);
                        q.corner[k][0] = px / g_w * 2 - 1, q.corner[k][1] = 1 - py / g_h * 2, q.corner[k][2] = 1;
                    }
                    // The gun's up (the turned window's) on this eye's screen: the reticle cants with the gun.
                    float ux = Dot(ax[1], nr), uy = Dot(ax[1], v.up), ul = sqrtf(ux * ux + uy * uy);
                    if (ul < 1e-3f) ux = 0, uy = 1, ul = 1;
                    if (g_sight[11] != 0) {  // sight4: its own fine turn, clockwise
                        float c = cosf(g_sight[11] * 0.0174533f), sn = sinf(g_sight[11] * 0.0174533f);
                        float nx = ux * c + uy * sn, ny = uy * c - ux * sn;
                        ux = nx, uy = ny;
                    }
                    q.color[3] = g_sight[7] < 0.02f ? 0.02f : g_sight[7] > 2 ? 2 : g_sight[7];  // sight4: no whole fade...
                    // ...instead each pixel fades by where its own line from the eye crosses the window.
                    const float hw = g_sight[3] * 0.5f > 0.002f ? g_sight[3] * 0.5f : 0.002f,
                                hh = g_sightShape ? (g_sight[4] * 0.5f > 0.002f ? g_sight[4] * 0.5f : 0.002f) : hw;
                    auto winAt = [&](float px, float py, float o[2]) {
                        float d[3], e2c[3];
                        const float ax_ = (px / g_w - 0.5f) * 2 * tx, ay_ = (0.5f - py / g_h) * 2 * ty;
                        for (int i = 0; i < 3; ++i) d[i] = v.fwd[i] + nr[i] * ax_ + v.up[i] * ay_, e2c[i] = C[i] - v.pos[i];
                        float dn = Dot(d, ax[2]);
                        if (fabsf(dn) < 1e-5f) dn = 1e-5f;
                        float t = Dot(e2c, ax[2]) / dn, x = 0, y = 0;
                        for (int i = 0; i < 3; ++i) {
                            float r = v.pos[i] + d[i] * t - C[i];
                            x += r * ax[0][i], y += r * ax[1][i];
                        }
                        o[0] = x / hw, o[1] = y / hh;
                    };
                    float w0[2], wx[2], wy[2];
                    winAt(sx, sy, w0), winAt(sx + qx, sy, wx), winAt(sx, sy - qy, wy);
                    q.win[0][0] = w0[0], q.win[0][1] = w0[1], q.win[0][2] = wx[0] - w0[0], q.win[0][3] = wx[1] - w0[1];
                    q.win[1][0] = wy[0] - w0[0], q.win[1][1] = wy[1] - w0[1], q.win[1][2] = (float)g_sightShape;
                    q.win[1][3] = 1 + (g_sight[5] < 0 ? 0 : g_sight[5] > 0.99f ? 0.99f : g_sight[5]);
                    const float pxMoa = tx / (0.5f * g_w) / moa;  // one pixel, minutes of angle
                    q.corner[0][3] = 4, q.corner[3][3] = (float)g_sightRet, q.corner[2][3] = ext;
                    q.corner[1][3] = pxMoa / sc, q.corner[3][2] = g_sight[9] * 0.25f / sc;  // thickness: half of half the dot size
                    q.corner[0][2] = g_sight[8], q.corner[1][2] = ux / ul, q.corner[2][2] = uy / ul;
                }
            }
        }
    };
    if (tur || (!thr && g_showDot)) dotQuad(false, tReal);
    if (win) dotQuad(true, tHit);
    if (nq) DrawQuads(quads, nq, nBeam);
    InterlockedIncrement(&g_drawn);
}

// holster2: marks on the body (hip spheres, chest throwable icons), facing this eye,
// a fixed size in the world, drawn on top like the beam. Positions in the game world.
void LaserDrawMarks(IDXGISwapChain *sc, const LaserMark *m, int n) {
    EngineView v;
    float tx, ty;
    if (n <= 0 || !EngineLastView(&v) || !XrGetHeadsetFov(&tx, &ty) || !Setup(sc)) return;
    float nr[3] = {v.fwd[1] * v.up[2] - v.fwd[2] * v.up[1], v.fwd[2] * v.up[0] - v.fwd[0] * v.up[2],
                   v.fwd[0] * v.up[1] - v.fwd[1] * v.up[0]};
    Cb quads[32];  // quiver1: was 12 (the quiver's marks were dropped); wrist1m: 32
    int nq = 0;
    for (int j = 0; j < n && nq < 32; ++j) {
        float p[3] = {m[j].pos[0] - v.pos[0], m[j].pos[1] - v.pos[1], m[j].pos[2] - v.pos[2]};
        float z = Dot(p, v.fwd);
        if (z < 0.08f) continue;
        float sx = (Dot(p, nr) / (z * tx) * 0.5f + 0.5f) * g_w, sy = (0.5f - Dot(p, v.up) / (z * ty) * 0.5f) * g_h;
        float rx = m[j].radius / (z * tx) * 0.5f * g_w, ry = m[j].radius / (z * ty) * 0.5f * g_h;
        Cb &q = quads[nq++];
        q = Cb{};
        for (int k = 0; k < 4; ++k) {
            float px = sx + ((k & 1) ? rx : -rx), py = sy + ((k >> 1) ? -ry : ry);
            q.corner[k][0] = px / g_w * 2 - 1, q.corner[k][1] = 1 - py / g_h * 2, q.corner[k][2] = 1;
        }
        q.corner[0][3] = 3, q.corner[3][3] = (float)m[j].icon;
        q.corner[2][3] = g_iconSrv ? 1.0f : 0.0f;  // c4icons1
        if (m[j].icon == 8) q.corner[1][3] = m[j].val;  // wrist1m
        if (m[j].icon == 8 && m[j].off[3] > 0.5f) memcpy(q.win[0], m[j].bg, 16), memcpy(q.win[1], m[j].off, 16);  // numcol2
        memcpy(q.color, m[j].rgba, 16);
    }
    if (nq) DrawQuads(quads, nq);
}

long LaserDrawn() {

    return InterlockedExchange(&g_drawn, 0);
}

// ray2: the game's own physics ray query, FUN_114505a0 in FC3_d3d11.dll, called the way
// gameplay code calls it (copied from FUN_117b0d30, a ground probe):
//   eax = &start xyz, esi = &delta xyz (end = start + delta), stack (callee pops 0x14):
//   world = *(*(0x1282a3d4) + 0x90), &results {data, count, flags}, &filter {vt 0x11f703a4,
//   mask, 0}, exclude list (0), mode (0 = all hits).
// Each result is 0x30 bytes: hit xyz at 0/4/8, normal at 0xc, collidable at 0x18, distance (m) at 0x1c. Unsorted.
// The results array is freed with the game's own helper FUN_11799a30 (eax = cleanup record),
// exactly as the game does after every query. combo2 used the editor export instead, whose
// hit loop reads an editor-only global (null in play) and crashed.
// Physics is only safe to ask from the game thread, so engine.cpp's on-foot camera update
// (game thread, once a frame) calls this with the beam the renderer last drew.
static const DWORD kImageBase = 0x10000000, kFilterMask = 0x25bf;
struct RayRes {
    BYTE *data;
    DWORD count, flags;
};
struct RayFilter {
    DWORD vt, mask, zero;
};
struct RayFree {
    DWORD vt;
    BYTE **data0, **data1;
    DWORD zero;
    DWORD *count;
    DWORD stride;
};
extern "C" void fc3vr_RayCall(DWORD fn, DWORD world, RayRes *res, RayFilter *flt, const float *start,
                              const float *delta) asm("fc3vr_RayCall");
extern "C" void fc3vr_RayFree(DWORD fn, RayFree *rec, BYTE **data, DWORD *count) asm("fc3vr_RayFree");
asm(".text\n.globl fc3vr_RayCall\nfc3vr_RayCall:\n"
    "  pushl %ebp\n  movl %esp, %ebp\n  pushl %ebx\n  pushl %esi\n  pushl %edi\n"
    "  movl 8(%ebp), %ebx\n"     // fn
    "  movl 28(%ebp), %esi\n"    // delta
    "  pushl $0\n  pushl $0\n"   // mode, exclude list
    "  pushl 20(%ebp)\n"         // filter
    "  pushl 16(%ebp)\n"         // results
    "  pushl 12(%ebp)\n"         // world
    "  movl 16(%ebp), %ecx\n"    // as the game leaves them: ecx = &results
    "  movl 24(%ebp), %eax\n"    // start
    "  call *%ebx\n"
    "  leal -12(%ebp), %esp\n  popl %edi\n  popl %esi\n  popl %ebx\n  popl %ebp\n  ret\n"
    ".globl fc3vr_RayFree\nfc3vr_RayFree:\n"
    "  pushl %ebp\n  movl %esp, %ebp\n  pushl %ebx\n  pushl %esi\n  pushl %edi\n"
    "  movl 8(%ebp), %ebx\n  movl 12(%ebp), %eax\n  movl 16(%ebp), %ecx\n  movl 20(%ebp), %edx\n"
    "  call *%ebx\n"
    "  leal -12(%ebp), %esp\n  popl %edi\n  popl %esi\n  popl %ebx\n  popl %ebp\n  ret\n");

static DWORD g_rQuery, g_rFree, g_rGlobal, g_rFilterVt, g_rArrVt;
void LaserGameTick() {
    bool tur = g_turBeam && EngineTurret();  // turret1
    if (!tur && ((!g_wallOn && !g_showDot && !g_sightOn) || !g_on || (!g_showBeam && !g_showDot && !g_sightOn))) return;
    static int s_state;  // 0 not looked up, 1 ready, -1 unavailable
    if (s_state == 0) {
        s_state = -1;
        // bd6: Blood Dragon. Same query, compiled as a plain thiscall: 0x1118a408 takes ecx = *(0x11f01ea0),
        // loads the world from +0x90 itself and jumps to 0x111a3e78 (start, delta, results, filter, 0, 0;
        // ret 18h). Filter {vt 0x11527354, mask, 0} as its builder 0x1118b2c7 writes it. Results are
        // 0x30-byte records, freed by 0x101c6462 (thiscall on the results array). Read from the ground
        // probe at 0x103e538d.
        if (g_bloodDragon && BdEngineOn() && BdStage() >= 6) {
            BYTE *bd = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll");
            if (bd) {
                auto at = [&](DWORD va) { return (DWORD)(bd + (va - kImageBase)); };
                static const BYTE kQ[] = {0x55, 0x8b, 0xec, 0x8b, 0x89, 0x90, 0x00, 0x00, 0x00, 0x5d, 0xe9};
                static const BYTE kQ2[] = {0x53, 0x8b, 0xdc, 0x51, 0x51, 0x83, 0xe4, 0xf0, 0x83, 0xc4, 0x04, 0x55};
                static const BYTE kF[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x18, 0x83, 0x65, 0xf4, 0x00};
                static const BYTE kF2[] = {0xc7, 0x45, 0xfc, 0x30, 0x00, 0x00, 0x00};  // record size 30h
                BYTE *q = (BYTE *)at(0x1118a408), *q2 = (BYTE *)at(0x111a3e78), *f = (BYTE *)at(0x101c6462);
                DWORD *fv = (DWORD *)at(0x11527354);
                bool ok = !IsBadReadPtr(q, 15) && !memcmp(q, kQ, 11) && q + 15 + *(int *)(q + 11) == q2 &&
                          !IsBadReadPtr(q2, 12) && !memcmp(q2, kQ2, 12) && !IsBadReadPtr(f, 0x20) && !memcmp(f, kF, 10) &&
                          !memcmp(f + 0x19, kF2, 7) && !IsBadReadPtr(fv, 4) && fv[0] > (DWORD)bd && fv[0] < (DWORD)bd + 0x1b00000;
                if (ok) g_rQuery = (DWORD)q, g_rFree = (DWORD)f, g_rGlobal = at(0x11f01ea0), g_rFilterVt = (DWORD)fv, s_state = 1;
            }
        }
        Log("laser: walls from the game's physics ray: %s", s_state > 0 ? "ready" : "not found (game changed?); beam not cut");
    }
    if (s_state < 0) return;
    float h[3], d[3], cam[3];
    DWORD tick;
    AcquireSRWLockShared(&g_rayLock);
    memcpy(h, g_ray.hand, 12), memcpy(d, g_ray.dir, 12), memcpy(cam, g_ray.cam, 12), tick = g_ray.tick;
    ReleaseSRWLockShared(&g_rayLock);
    DWORD now = GetTickCount();
    if (!tick || now - tick > 200) return;  // no beam drawn lately (menu, vehicle, cutscene)
    float t0 = g_start + 0.05f, t1 = g_len > 0.1f ? g_len : 0.1f;
    if ((g_showDot || g_sightOn) && g_dotRange > t1) t1 = g_dotRange;  // sight5  // dot3: the glow can reach past the beam
    if (tur) t0 = 1.5f, t1 = g_turDotRange > 150 ? g_turDotRange : 150.0f;  // turret1: start past the gun's own shield (lasers1: to 250 m)
    t1 += 1.0f;
    float len = t1 - t0;
    if (len <= 0.05f) return;
    // The physics world, read fresh each time (it changes with loads): skip if anything is missing.
    DWORD g = *(volatile DWORD *)g_rGlobal, world = 0;
    if (g && !IsBadReadPtr((void *)(g + 0x90), 4)) world = *(DWORD *)(g + 0x90);
    if (!world || IsBadReadPtr((void *)world, 0x10) || !*(DWORD *)(world + 0xc) ||
        IsBadReadPtr((void *)*(DWORD *)(world + 0xc), 0x340)) {
        g_wDist = -1, g_wTick = now;
        return;
    }
    alignas(16) float o[4] = {h[0] + d[0] * t0, h[1] + d[1] * t0, h[2] + d[2] * t0, 0};
    alignas(16) float del[4] = {d[0] * len, d[1] * len, d[2] * len, 0};
    RayRes res = {nullptr, 0, 0};
    RayFilter flt = {g_rFilterVt, kFilterMask, 0};
    if (g_bloodDragon)
        BdThisCall((void *)g_rQuery, (void *)g, 6, o, del, &res, &flt, 0, 0);
    else
        fc3vr_RayCall(g_rQuery, world, &res, &flt, o, del);
    // Nearest hit that isn't the player's own body (a hit inside a 0.45 m upright column
    // under the head, from 2 m below it to 0.3 m above).
    float best = -1;
    DWORD n = res.count;
    if (res.data && n && n < 4096) {
        for (DWORD i = 0; i < n; ++i) {
            const float *rec = (const float *)(res.data + i * 0x30);
            // ray4: +0x1c is the distance along the ray in metres (ray3 log: it equals the hit's
            // distance from the start), not a 0-1 fraction; use the hit point itself to be sure.
            float rel[3] = {rec[0] - o[0], rec[1] - o[1], rec[2] - o[2]};
            float fr = Dot(rel, d);
            if (!(fr > 0.001f && fr < len)) continue;
            float dx = rec[0] - cam[0], dy = rec[1] - cam[1], dz = rec[2] - cam[2];
            if (dx * dx + dy * dy < 0.45f * 0.45f && dz > -2.0f && dz < 0.3f) continue;
            if (best < 0 || fr < best) best = fr;
        }
    }
    if (res.data && g_bloodDragon) {
        BdThisCall((void *)g_rFree, &res, 0);
    } else if (res.data) {
        RayFree fr = {g_rArrVt, &res.data, &res.data, 0, &res.count, 0x30};
        fc3vr_RayFree(g_rFree, &fr, &res.data, &res.count);
    }
    bool got = best > 0;
    g_wDist = got ? t0 + best : -1, g_wTick = now;
}

// ---- slowfx1 (DrGonzaga 16 Nov): the slow motion look (slowfx.cpp), into this eye's image ----
// A pixel shader and two blend states of their own, made the first time they are wanted: if any of it can't be
// made the log says so and only this look is missing. Both blends leave the image's alpha as it was.
namespace {
const char kFxPs[] =
    "cbuffer C : register(b0) { float4 corner[4]; float4 color; float4 win[2]; float4 dz; float4 occ; };\n"
    "struct V { float4 p : SV_Position; float a : A; float2 uv : U; };\n"
    "float4 ps(V i) : SV_Target {\n"
    // the ring round the clock: win[1].z = 1, win[1].w = how much of it is left (from the top, clockwise)
    // slowglow1: win[1].z = 2, a soft round glow, brightest in the middle
    "  if (win[1].z > 1.5) {\n"
    "    float r = length(i.uv);\n"
    "    float g = exp(-r * r * 4.0) * (1 - smoothstep(0.75, 1.0, r));\n"
    "    return float4(color.rgb, color.a * g); }\n"
    "  if (win[1].z > 0.5) {\n"
    "    float r = length(i.uv);\n"
    "    float a = frac(atan2(i.uv.x, i.uv.y + 0.00001) / 6.2831853 + 1.0);\n"
    "    float d = abs(r - 0.82) - 0.06;\n"
    "    float ink = 1 - smoothstep(0.0, 0.04, d), glow = exp(-max(d, 0) * 14) * 0.45;\n"
    "    float on = a <= win[1].w ? 1.0 : 0.0;\n"
    "    return float4(color.rgb, color.a * on * max(ink, glow)); }\n"
    // the whole picture: color = the vignette's colour and strength, win[0] = where it starts (0 centre .. 1 edge),
    // scan line strength, scan lines down the picture; win[1].xy = the picture's shape (the wider side = 1)
    "  float r = length(i.uv * win[1].xy);\n"
    "  float v = saturate(smoothstep(win[0].x, 1.05, r) * color.a);\n"
    "  float s = win[0].y * (0.5 - 0.5 * cos(i.uv.y * win[0].z * 3.14159265));\n"
    "  float al = v + s - v * s;\n"
    "  float3 c = al > 0.0001 ? color.rgb * v * (1 - s) / al : color.rgb;\n"
    "  return float4(c, al); }\n";
ID3D11PixelShader *g_fxPs;
ID3D11BlendState *g_fxMul, *g_fxOver;
bool g_fxFailed;
bool FxSetup() {
    if (g_fxFailed) return false;
    if (g_fxPs) return true;
    HMODULE dc = GetModuleHandleW(L"d3dcompiler_47.dll");
    CompileFn compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
    ID3DBlob *psb = nullptr, *err = nullptr;
    if (compile && SUCCEEDED(compile(kFxPs, sizeof kFxPs - 1, "slowfx", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err)))
        g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_fxPs);
    if (!g_fxPs) Log("slowfx: shader not made (%.300s); no slow motion look", err ? (const char *)err->GetBufferPointer() : "no compiler");
    if (psb) psb->Release();
    if (err) err->Release();
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].BlendOp = bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO, bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR, bld.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;  // picture * (rgb + a)
    g_dev->CreateBlendState(&bld, &g_fxMul);
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA, bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    g_dev->CreateBlendState(&bld, &g_fxOver);
    if (!g_fxPs || !g_fxMul || !g_fxOver) return g_fxFailed = true, false;
    Log("slowfx: slow motion look ready");
    return true;
}
// One quad with the given shader and blend, the game's state saved and put back around it.
void FxQuad(const Cb &q, ID3D11PixelShader *ps, ID3D11BlendState *bs) {
    ID3D11DeviceContext *c = g_ctx;
    HeadtrackOwnDraw(true);
    D3DState game(c, 1, 1);
    D3D11_VIEWPORT vp = {0, 0, (float)g_w, (float)g_h, 0, 1};
    EngineEyeViewport(&vp.TopLeftX, &vp.TopLeftY, &vp.Width, &vp.Height);
    c->RSSetViewports(1, &vp);
    c->RSSetState(g_rs);
    c->OMSetDepthStencilState(g_ds, 0);
    float bf[4] = {0, 0, 0, 0};
    c->OMSetBlendState(bs, bf, 0xffffffff);
    c->OMSetRenderTargets(1, &g_rtv, nullptr);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(g_vs, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &g_cb);
    c->PSSetShader(ps, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &g_cb);
    c->UpdateSubresource(g_cb, 0, nullptr, &q, 0, 0);
    c->Draw(4, 0);
    game.Restore();
    HeadtrackOwnDraw(false);
}
}  // namespace

void LaserDrawFx(IDXGISwapChain *sc, const LaserFx &fx) {
    float tx, ty;
    if (!Setup(sc) || !FxSetup() || !XrGetHeadsetFov(&tx, &ty)) return;
    Cb q = {};
    for (int k = 0; k < 4; ++k) q.corner[k][0] = (k & 1) ? 1.f : -1.f, q.corner[k][1] = (k >> 1) ? 1.f : -1.f, q.corner[k][2] = 1;
    // the colour push: picture * gain, as picture * (rgb + a) with a = the smallest gain (at most 1)
    float a = fminf(1.f, fminf(fx.gain[0], fminf(fx.gain[1], fx.gain[2])));
    if (a < 0) a = 0;
    if (fabsf(fx.gain[0] - 1) > 0.003f || fabsf(fx.gain[1] - 1) > 0.003f || fabsf(fx.gain[2] - 1) > 0.003f) {
        for (int i = 0; i < 3; ++i) q.color[i] = fx.gain[i] - a < 0 ? 0 : fx.gain[i] - a;
        q.color[3] = a;
        FxQuad(q, g_ps, g_fxMul);  // the main shader's plain quad (corner[0].w = 0): the colour as it is
    }
    if (fx.vig[3] > 0.003f || fx.scan > 0.003f) {
        memcpy(q.color, fx.vig, 16);
        float mx = tx > ty ? tx : ty;
        q.win[0][0] = fx.vigStart, q.win[0][1] = fx.scan, q.win[0][2] = fx.scanLines;
        q.win[1][0] = tx / mx, q.win[1][1] = ty / mx, q.win[1][2] = 0;
        FxQuad(q, g_fxPs, g_fxOver);
    }
}

// A ring facing this eye, centred at a point in the game world, 'frac' of it shown.
static void DrawRingQuad(IDXGISwapChain *sc, const float *pos, float radius, float frac, const float *rgba, float mode);
void LaserDrawRing(IDXGISwapChain *sc, const float *pos, float radius, float frac, const float *rgba) { DrawRingQuad(sc, pos, radius, frac, rgba, 1); }
void LaserDrawGlow(IDXGISwapChain *sc, const float *pos, float radius, const float *rgba) { DrawRingQuad(sc, pos, radius, 1, rgba, 2); }
static void DrawRingQuad(IDXGISwapChain *sc, const float *pos, float radius, float frac, const float *rgba, float mode) {
    EngineView v;
    float tx, ty;
    if (!EngineLastView(&v) || !XrGetHeadsetFov(&tx, &ty) || !Setup(sc) || !FxSetup()) return;
    float nr[3] = {v.fwd[1] * v.up[2] - v.fwd[2] * v.up[1], v.fwd[2] * v.up[0] - v.fwd[0] * v.up[2],
                   v.fwd[0] * v.up[1] - v.fwd[1] * v.up[0]};
    float p[3] = {pos[0] - v.pos[0], pos[1] - v.pos[1], pos[2] - v.pos[2]};
    float z = Dot(p, v.fwd);
    if (z < 0.08f) return;
    float sx = (Dot(p, nr) / (z * tx) * 0.5f + 0.5f) * g_w, sy = (0.5f - Dot(p, v.up) / (z * ty) * 0.5f) * g_h;
    float rx = radius / (z * tx) * 0.5f * g_w, ry = radius / (z * ty) * 0.5f * g_h;
    Cb q = {};
    for (int k = 0; k < 4; ++k) {  // as LaserDrawMarks
        float px = sx + ((k & 1) ? rx : -rx), py = sy + ((k >> 1) ? -ry : ry);
        q.corner[k][0] = px / g_w * 2 - 1, q.corner[k][1] = 1 - py / g_h * 2, q.corner[k][2] = 1;
    }
    memcpy(q.color, rgba, 16);
    q.win[1][2] = mode, q.win[1][3] = frac;
    FxQuad(q, g_fxPs, g_fxOver);
}
