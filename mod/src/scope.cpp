// Scope lens (scope1, 31 Oct). While a scoped gun is aimed (the game's zoom,
// engine.cpp), the right eye's render is the scope view: along the gun at the
// game's zoom. At that eye's Present its image is copied here; at the next left
// eye's Present it is drawn into the left image as a round lens around where
// the beam points, and the headset shows the left image to both eyes (xr.cpp),
// so the lens looks like a scope's picture: collimated, at the aim point.
// Drawn on top of everything (no depth). States we change are put back.
// pip1 (21 Nov, [aim] ScopeStereo): the scope view is a third render each frame instead (eye 2, drawn before the
// eyes by stereo.cpp); its image is copied here and the lens is drawn into BOTH eyes, each from its own place, so
// the world around the lens stays 3D.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "fc3vr.h"

namespace {

const char kShader[] =
    "cbuffer C : register(b0) { float4 corner[4]; float4 opt; float4 col; float4 sty; float4 rot; float4 fx; float4 fx2;\n"
    "  float4 rf; float4 rA; float4 rR; float4 rU; float4 rB; };\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    // rifle1: ray = the view-space point on the quad (x right, y up, z ahead), for the eyepiece's per-pixel view direction.
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD; float3 ray : TEXCOORD1; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.p = corner[id]; o.uv = float2(id & 1, id >> 1);\n"
    "  o.ray = float3(corner[id].x * rf.y, corner[id].y * rf.z, corner[id].w); return o; }\n"
    // Line / dot coverage, anti-aliased by the pixel footprint (f = lens units per pixel).
    // glow1 (DrGonzaga 2 Nov): a soft halo around the lines and dots, gk strong (rot.z).
    "static float gk = 0;\n"
    "float Ln(float dist, float hw, float f) { float h = saturate(1 - (dist - hw) / 0.014);\n"
    "  return max(saturate((hw - dist) / f + 0.5), gk * h * h * h); }\n"
    // ret3 (DrGonzaga 6 Nov, new reticles): Lb = a fine line with no glow (etched, black),
    // Lg = an illuminated part with a wider, softer glow.
    "float Lb(float dist, float hw, float f) { return saturate((hw - dist) / f + 0.5); }\n"
    "float Lg(float dist, float hw, float f) { float h = saturate(1 - (dist - hw) / 0.024);\n"
    "  return max(saturate((hw - dist) / f + 0.5), gk * h * h); }\n"
    // Distance from v to the nearest multiple of step.
    "float Grid(float v, float step) { return abs(frac(v / step + 0.5) - 0.5) * step; }\n"
    "float Sg(float2 p, float2 a, float2 b) { float2 pa = p - a, ba = b - a; return length(pa - ba * saturate(dot(pa, ba) / dot(ba, ba))); }\n"
    // opt.x: how much of the scope image's height the lens shows (1 = all), opt.y: its
    // width / height, opt.zw: reticle offset. col: reticle colour and opacity.
    "float4 ps(V i) : SV_Target {\n"
    // square1 (DrGonzaga 15 Nov, Blood Dragon: one sniper rifle has a square viewport): fx.x > 0.5 = a square
    // lens, its sides turned with the gun (rot.xy), sized so a turned square stays inside the quad.
    // square2 (DrGonzaga 15 Nov: "slightly rounded edges"): fx2.x = corner radius as a part of the half side.
    // rifle1 (DrGonzaga 21 Nov: "attached to the rifle, with an eye box, that you peer through- like a real rifle"):
    // rf.x > 0.5 = the quad is the eyepiece glass on the gun. The lens position comes from the direction this pixel is
    // seen in, against the scope's axis (rA.xyz; rA.w = 1 / tan of the lens's half-angle; rR/rU = the picture's right
    // and up), like a real scope: the picture is at infinity, so it stays put as the head moves. Eye box: the field
    // seen is a circle at rB.xy (moves with the eye's offset from the axis) of radius rB.z (shrinks when the eye is too
    // near or far), soft edge rB.w; outside it the tube's black.
    "  float2 d = i.uv * 2 - 1; float vis = 1;\n"
    // squarerifle1 (DrGonzaga 22 Nov: "can we have the option for a square scope?"): with Square on, the glass, the eye
    // box's field and the picture are rounded squares (fx2.x corners), lined up with the gun.
    "  float rq = clamp(fx2.x, 0, 0.9);\n"
    "  if (rf.x > 0.5) { float g = length(d);\n"
    "    if (fx.x > 0.5) { float cs = cos(rf.w), sn = sin(rf.w); float2 eg = d * float2(1, -1);\n"
    "      eg = float2(dot(eg, float2(cs, -sn)), dot(eg, float2(sn, cs)));\n"
    "      float2 mg = abs(eg) - (1 - rq); g = (1 - rq) + length(max(mg, 0)) + min(max(mg.x, mg.y), 0); }\n"
    "    clip(1 - g); float3 rr = normalize(i.ray); float z = dot(rr, rA.xyz);\n"
    "    clip(z - 0.01); d = float2(dot(rr, rR.xyz), -dot(rr, rU.xyz)) / z * rA.w;\n"
    "    float2 fb = d - rB.xy; float fr = length(fb);\n"
    "    if (fx.x > 0.5) { fb *= float2(1, -1); fb = float2(dot(fb, float2(rot.y, -rot.x)), dot(fb, rot.xy));\n"
    "      float rz = max(rB.z, 0.001), rr2 = rq * rz; float2 mb = abs(fb) - (rz - rr2);\n"
    "      fr = (rz - rr2) + length(max(mb, 0)) + min(max(mb.x, mb.y), 0); }\n"
    "    vis = (1 - smoothstep(rB.z - rB.w, rB.z, fr)) * smoothstep(1.0, 0.94, g); }\n"
    "  float r = length(d); float2 p = 0;\n"
    "  if (fx.x > 0.5) { float2 e2 = d * float2(1, -1); e2 = float2(dot(e2, float2(rot.y, -rot.x)), dot(e2, rot.xy));\n"
    "    p = rf.x > 0.5 ? e2 : e2 / 0.7; float rc = rq; float2 m = abs(p) - (1 - rc);\n"
    "    r = (1 - rc) + length(max(m, 0)) + min(max(m.x, m.y), 0); }\n"
    "  if (rf.x < 0.5) clip(1 - r);\n"
    // lens1 (DrGonzaga 6 Nov): rim distortion (rot.w strength), centre unchanged, soft darkening at the rim.
    // lens2 (DrGonzaga 8 Nov): an edge effect: the middle ~55% of the lens is left
    // alone, the bending grows quickly in the outer band, with a mild colour fringe
    // there (red bent a little less, blue a little more).
    // lens4 (DrGonzaga 8 Nov): the edge samples move IN towards the middle (the rim is
    // stretched), so they never leave the scope image: lens3 sampled outside it at low
    // zoom, where the clamped edge showed as square corners and flat colour. The red /
    // blue split grows smoothly with the radius (round, no hard start) and blends back
    // to plain colour in the last few % of the rim.
    "  float e = smoothstep(0.4, 1.0, r); e = e * e;\n"
    "  float k = min(rot.w * 0.2 * e, 0.4), ca = min(rot.w * 0.022 * e, k * 0.5);\n"
    "  float2 sc = 0.5 * opt.x * float2(1 / opt.y, 1);\n"
    "  float2 dg = d * (1 - k);\n"
    "  float3 cg = t.Sample(s, 0.5 + sc * dg).rgb, c = cg;\n"
    "  c.r = t.Sample(s, 0.5 + sc * d * (1 - k + ca)).r;\n"
    "  c.b = t.Sample(s, 0.5 + sc * d * (1 - k - ca)).b;\n"
    "  c = lerp(cg, c, 1 - smoothstep(0.9, 1.0, r) * 0.7);\n"
    "  c *= 1 - 0.35 * saturate(rot.w) * smoothstep(0.6, 1.0, r);\n"
    // square2 (DrGonzaga 15 Nov: "a 'tv-style' lens distortion option with peripheral chromatic aberration (not round
    // like a round scope- like an older CRT TV style)"): fx2.y > 0 = the square lens bulges like a tube TV instead
    // (each side bows on its own: x grows with y squared and the other way round; scaled so the corners never
    // sample past the round lens's edge), red and green pulled in a little more towards the sides and corners
    // (fx2.z), and the corners darkened.
    "  if (fx.x > 0.5 && fx2.y > 0.001) { float kc = fx2.y * 0.35;\n"
    "    float2 pp = p * (1 + kc * p.yx * p.yx) / (1 + kc);\n"
    "    float cc = fx2.z * 0.035 * dot(p, p) * 0.5;\n"
    "    float2 dc = (pp.x * float2(rot.y, -rot.x) + pp.y * rot.xy) * float2(0.7, -0.7);\n"
    "    c.g = t.Sample(s, 0.5 + sc * dc * (1 - cc)).g;\n"
    "    c.r = t.Sample(s, 0.5 + sc * dc * (1 - 2 * cc)).r;\n"
    "    c.b = t.Sample(s, 0.5 + sc * dc).b;\n"
    "    float vg = saturate((1 - p.x * p.x) * (1 - p.y * p.y)); c *= lerp(1, pow(vg, 0.3), 0.7 * saturate(fx2.y)); }\n"
    // square1: inside the lens only: an orange tint (fx.z), and scan lines (fx.y) that drift slowly down
    // with a faint brighter band sweeping through (fx.w = seconds).
    "  c = lerp(c, dot(c, float3(0.35, 0.5, 0.15)) * float3(1.45, 0.82, 0.30) * 0.6 + c * float3(1.1, 0.7, 0.35) * 0.4, saturate(fx.z));\n"
    // scanrot1 (DrGonzaga 15 Nov: "make the scope scanlines rotate with the scope"): measured along the gun's own
    // up (rot.xy) instead of the screen's.
    "  if (fx.y > 0.001) { float sy = -dot(d * float2(1, -1), rot.xy);\n"
    "    float ln = 0.5 + 0.5 * sin((sy * 55 - fx.w * 1.5) * 6.2832);\n"
    "    float band = saturate(1 - abs(frac(sy * 0.5 - fx.w * 0.12) - 0.5) * 12);\n"
    "    c *= 1 - fx.y * 0.45 * ln; c += fx.y * 0.10 * band * (c + 0.03); }\n"
    "  float f = max(fwidth(d.x), 1e-4);\n"
    // scope5: long-range reticle (DrGonzaga's picture): fine cross with a centre gap
    // and mil-style ticks, holdover dots below, thin black ring.
    "  float2 q = (d - opt.zw) * float2(1, -1); gk = sty.x > 2.5 && sty.x < 3.5 ? 0 : rot.z;\n"
    // scope12: the reticle turns with the gun (rot.xy = its up in lens coordinates).
    "  q = float2(dot(q, float2(rot.y, -rot.x)), dot(q, rot.xy));\n"
    "  float ax = abs(q.x), ay = abs(q.y), hw = max(0.0021, f * 0.42), m = 0;\n"
    // ret2 (DrGonzaga 1 Nov): red and green reticles sharper: thinner lines, smaller dots.
    "  float th = max(0.0013, f * 0.3);\n"
    // scope7: sty.x reticle style: 0 MRAD (long range, beam red), 1 small blue dot,
    // 2 prism style (DrGonzaga's Midas TSP3 picture): ring open at the bottom, tiny
    // dot, side stadia with ticks, drop line with ticks and a thick post. sty.yzw: colour.
    // 3: black duplex (DrGonzaga's 4th picture): tapered posts from the edge, a fine
    // ticked cross between them, a small centre square, a dark edge.
    // ret3 (DrGonzaga 6 Nov): our own designs, fine etched black lines with only the
    // illuminated parts lit and glowing (colour sty.yzw). 4 amber chevron with a drop
    // line and ticks, 5 green ring and dot, 6 ruby lit centre cross, 7 cyan mil-dots.
    // future1 (DrGonzaga 12 Nov, Blood Dragon): four futuristic scope reticles, all lit (no etched black):
    // 8 neon brackets, 9 cyber hex with drop ticks, 10 tri-prong, 11 scanner (three arcs, side ticks).
    "  if (sty.x > 7.5) { float l = 0, rq = length(q); float2 a = float2(ax, ay);\n"
    "    if (sty.x < 8.5) {\n"
    "      l = Lg(min(Sg(a, float2(0.22, 0.22), float2(0.22, 0.13)), Sg(a, float2(0.22, 0.22), float2(0.13, 0.22))), th * 1.3, f);\n"
    "      l = max(l, Lg(Sg(a, float2(0.30, 0), float2(0.36, 0)), th, f));\n"
    "      l = max(l, Lg(rq, th * 1.6, f)); }\n"
    "    else if (sty.x < 9.5) {\n"
    "      l = Lg(abs(max(ax * 0.866 + ay * 0.5, ay) - 0.2), th, f);\n"
    "      l = max(l, Lg(Sg(a, float2(0.27, 0), float2(0.35, 0)), th, f));\n"
    "      if (q.y < -0.25 && q.y > -0.62) l = max(l, Lg(Grid(-q.y, 0.07), th, f) * Lb(ax, 0.02, f));\n"
    "      l = max(l, Lg(rq, th * 1.5, f)); }\n"
    "    else if (sty.x < 10.5) {\n"
    "      l = Lg(min(Sg(q, float2(0, -0.05), float2(0, -0.30)),\n"
    "        min(Sg(q, float2(0.0433, 0.025), float2(0.26, 0.15)), Sg(q, float2(-0.0433, 0.025), float2(-0.26, 0.15)))), th * 1.4, f);\n"
    "      l = max(l, Lg(rq, th * 1.2, f)); }\n"
    "    else { float g = abs(frac(atan2(q.x, q.y) / 2.0944 + 0.5) - 0.5);\n"
    "      if (g > 0.07) l = Lg(abs(rq - 0.24), th * 1.2, f);\n"
    "      l = max(l, Lg(abs(rq - 0.04), th, f));\n"
    "      if (ax > 0.30 && ax < 0.52) l = max(l, Lg(Grid(ax, 0.05), th, f) * Lb(ay, 0.015, f));\n"
    "      l = max(l, Lg(rq, th * 0.9, f)); }\n"
    "    c = lerp(c, sty.yzw, saturate(l) * col.a); }\n"
    "  else if (sty.x > 3.5) {\n"
    "    float k = 0, l = 0, fw = max(0.0009, f * 0.26), rq = length(q);\n"
    "    if (sty.x < 4.5) {\n"
    "      if (ax > 0.07) k = max(k, Lb(ay, fw, f));\n"
    "      if (q.y < -0.07) k = max(k, Lb(ax, fw, f));\n"
    "      if (ax > 0.6) k = max(k, Lb(ay, fw * 3.5, f));\n"
    "      if (q.y < -0.6) k = max(k, Lb(ax, fw * 3.5, f));\n"
    "      if (q.y < -0.1 && q.y > -0.52) { float big = Grid(-q.y, 0.12) < 0.01 ? 0.03 : 0.016;\n"
    "        k = max(k, Lb(Grid(-q.y, 0.06), fw, f) * Lb(ax, big, f)); }\n"
    "      float2 a = float2(ax, -q.y), dv = float2(0.6, 0.8);\n"
    "      float tt = clamp(dot(a, dv), 0, 0.05);\n"
    "      l = max(l, Lg(length(a - dv * tt), th * 1.2, f)); }\n"
    "    else if (sty.x < 5.5) {\n"
    "      if (ax > 0.17) k = max(k, Lb(ay, fw, f));\n"
    "      if (ay > 0.17) k = max(k, Lb(ax, fw, f));\n"
    "      if (ax > 0.62) k = max(k, Lb(ay, fw * 3.5, f));\n"
    "      if (ay > 0.62) k = max(k, Lb(ax, fw * 3.5, f));\n"
    "      l = max(l, Lg(abs(rq - 0.13), th, f));\n"
    "      l = max(l, Lg(rq, th * 2.2, f)); }\n"
    "    else if (sty.x < 6.5) {\n"
    "      if (ax > 0.055) k = max(k, Lb(ay, fw, f));\n"
    "      if (ay > 0.055) k = max(k, Lb(ax, fw, f));\n"
    "      if (ax > 0.5) k = max(k, Lb(ay, fw * 3.5, f));\n"
    "      if (ay > 0.5) k = max(k, Lb(ax, fw * 3.5, f));\n"
    "      if (ax > 0.012 && ax < 0.055) l = max(l, Lg(ay, th, f));\n"
    "      if (ay > 0.012 && ay < 0.055) l = max(l, Lg(ax, th, f));\n"
    "      l = max(l, Lg(rq, th * 1.2, f)); }\n"
    "    else {\n"
    "      k = max(k, Lb(ay, fw, f) * step(0.01, ax));\n"
    "      k = max(k, Lb(ax, fw, f) * step(0.01, ay));\n"
    "      if (ax > 0.56) k = max(k, Lb(ay, fw * 3.5, f));\n"
    "      if (ay > 0.56) k = max(k, Lb(ax, fw * 3.5, f));\n"
    "      if (ax < 0.5 && ay < 0.02) l = max(l, Lg(length(float2(Grid(ax, 0.08), ay)), 0.0055, f));\n"
    "      if (ay < 0.5 && ax < 0.02) l = max(l, Lg(length(float2(ax, Grid(ay, 0.08))), 0.0055, f));\n"
    "      l = max(l, Lg(rq, th * 1.3, f)); }\n"
    "    c = lerp(c, float3(0.02, 0.02, 0.02), k * col.a);\n"
    "    c = lerp(c, sty.yzw, saturate(l) * col.a); }\n"
    "  else if (sty.x > 2.5) {\n"
    "    float k = 0, pw = 0.012;\n"
    "    float tip = 0.33;\n"
    "    if (q.y > tip) k = max(k, Ln(ax, pw * saturate((q.y - tip) / 0.08), f));\n"
    "    if (ax > tip && ay < pw + f) k = max(k, Ln(ay, pw * saturate((ax - tip) / 0.08), f));\n"
    "    if (ax < tip && ax > 0.03) { k = max(k, Ln(ay, hw, f));\n"
    "      float big = Grid(ax, 0.07) < 0.004 ? 0.022 : 0.011;\n"
    "      k = max(k, Ln(Grid(ax, 0.035), hw, f) * Ln(ay, big, f)); }\n"
    "    if (q.y < tip && ay > 0.03) { k = max(k, Ln(ax, hw, f));\n"
    "      float big = Grid(ay, 0.07) < 0.004 ? 0.022 : 0.011;\n"
    "      k = max(k, Ln(Grid(ay, 0.035), hw, f) * Ln(ax, big, f)); }\n"
    "    float sq = max(ax, ay); k = max(k, Ln(abs(sq - 0.02), hw, f));\n"
    "    c *= lerp(1.0, 0.3, smoothstep(0.55, 0.97, r));\n"
    "    c *= saturate((0.95 - r) / f);\n"
    "    c = lerp(c, float3(0, 0, 0), k * col.a); }\n"
    "  else if (sty.x > 1.5) {\n"
    "    float rq = length(q), w2 = th * 1.6;\n"
    "    if (!(q.y < 0 && ax < 0.05)) m = max(m, Ln(abs(rq - 0.1), w2 * 1.6, f));\n"
    "    m = max(m, Ln(rq, 0.004, f));\n"
    "    if (ax > 0.15 && ax < 0.302) {\n"
    "      m = max(m, Ln(ay, w2, f));\n"
    "      m = max(m, Ln(min(abs(ax - 0.2), min(abs(ax - 0.25), abs(ax - 0.3))), w2, f) * Ln(ay, 0.018, f)); }\n"
    "    if (q.y < -0.05 && q.y > -0.6) {\n"
    "      m = max(m, Ln(ax, w2, f));\n"
    "      float g = Grid(-q.y, 0.05), big = Grid(-q.y, 0.2) < 0.01 ? 0.04 : 0.02;\n"
    "      if (-q.y > 0.07) m = max(m, Ln(g, w2, f) * Ln(ax, big, f)); }\n"
    "    if (q.y <= -0.6 && q.y > -0.9) m = max(m, Ln(ax, 0.0055, f));\n"
    "    c = lerp(c, sty.yzw, m * col.a); m = 0; }\n"
    "  else if (sty.x > 0.5) {\n"
    "    float b = Ln(length(q), 0.007, f);\n"
    "    c = lerp(c, sty.yzw, b * col.a); }\n"
    "  else {\n"
    "  m = max(m, Ln(length(q), th * 1.5, f));\n"  // scope10: centre dot (scope11: 50% larger)
    "  if (ax > 0.03) m = max(m, Ln(ay, th, f));\n"
    "  if (ay > 0.03) m = max(m, Ln(ax, th, f));\n"
    "  if (ax > 0.03 && ax < 0.95) { float tl = Grid(ax, 0.1) < 0.002 ? 0.04 : 0.018;\n"
    "    m = max(m, Ln(Grid(ax, 0.05), th, f) * Ln(ay, tl, f)); }\n"
    "  if (ay > 0.03 && ay < 0.95) { float tl = Grid(ay, 0.1) < 0.002 ? 0.04 : 0.018;\n"
    "    m = max(m, Ln(Grid(ay, 0.05), th, f) * Ln(ax, tl, f)); }\n"
    "  if (q.y < -0.04 && q.y > -0.62) {\n"
    "    float row = round(-q.y / 0.075), ry = abs(-q.y - row * 0.075);\n"
    "    float hx = 0.06 + row * 0.028;\n"
    "    if (row >= 1 && ax > 0.03 && ax < hx) {\n"
    "      float dx = Grid(ax, 0.025), big = Grid(ax, 0.1) < 0.013 ? 0.006 : 0.0035;\n"
    "      m = max(m, Ln(length(float2(dx, ry)), big, f)); } }\n"
    "  }\n"
    "  c = lerp(c, col.rgb, m * col.a);\n"
    "  c *= saturate((0.985 - r) / f);\n"
    "  return float4(c * vis, 1); }\n";

// Fallback if the reticle shader doesn't compile: the plain lens.
const char kShaderPlain[] =
    "cbuffer C : register(b0) { float4 corner[4]; float4 opt; float4 col; };\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.p = corner[id]; o.uv = float2(id & 1, id >> 1); return o; }\n"
    "float4 ps(V i) : SV_Target {\n"
    "  float2 d = i.uv * 2 - 1; float r = length(d); clip(1 - r);\n"
    "  float2 uv = 0.5 + 0.5 * opt.x * float2(d.x / opt.y, d.y);\n"
    "  return float4(t.Sample(s, uv).rgb * saturate((1 - r) / 0.03), 1); }\n";

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                   UINT, UINT, ID3DBlob **, ID3DBlob **);

struct Cb {
    float corner[4][4];
    float opt[4];
    float col[4];
    float sty[4];
    float rot[4];  // scope12: the reticle's up in lens coordinates (x right, y up), from the gun
    float fx[4];   // square1: square lens, scan lines 0-1, orange tint 0-1, seconds
    float fx2[4];  // square2: corner radius, CRT bend 0-1, colour fringe 0-1, unused
    float rf[4], rA[4], rR[4], rU[4], rB[4];  // rifle1: see the shader
};

float g_size = 14.0f;  // lens radius, degrees (Aim page)
float g_reticle[2];   // reticle offset right, up (Aim page)
float g_zoom = 2.0f;
int g_style;  // reticle style: 0 long range, 1 simple, 2 red dot (per gun)
wchar_t g_gunName[64];  // the gun these scope settings are for
float g_retTurn = 0;  // scope12b: per-gun reticle turn, degrees clockwise (Aim page)
float g_glow = 0.3f;  // glow1: reticle glow strength (Aim page, all guns)
float g_distort = 0.3f;  // lens1: lens distortion strength 0-1 (all guns)
bool g_square;          // square1: per gun, [scope.<gun>] Square
float g_scan, g_tint;   // square1: per gun, Scanlines / OrangeTint 0-1
float g_corner = 0.12f, g_crt, g_fringe = 0.5f;  // square2: per gun, Corner / CrtBend / CrtFringe
// rifle1: per gun, [scope.<gun>] RifleScope, EyeRight, EyeUp, EyeForward, GlassSize, EyeBox, EyeDistance, EyeRange,
// rifle2: PictureFill (1 = the picture's edge, with its distortion, square, corners and CRT, meets the glass's edge
// seen from the best eye distance).
bool g_rifle;
// autoscope2 (DrGonzaga 21 Nov): when the auto scope turns on, per gun: AutoReach (m past the far end of the eye
// distance leeway), AutoSideways (m off the scope's axis), AutoAngleDeg (head direction vs the barrel). It turns off
// again a little further out (reach + 4 cm, sideways x 1.6, angle + 10 deg) so it does not flicker at the edge.
// fade1 (DrGonzaga 21 Nov: "fade in/fade out, quickly? Adjustable?"): per gun FadeIn / FadeOut, seconds (0 = at once).
float g_rv[13] = {0, 0.075f, -0.03f, 0.038f, 0.012f, 0.07f, 0.04f, 1.0f, 0.06f, 0.05f, 40.0f, 0.10f, 0.10f};
static const float kRvDef[13] = {0, 0.075f, -0.03f, 0.038f, 0.012f, 0.07f, 0.04f, 1.0f, 0.06f, 0.05f, 40.0f, 0.10f, 0.10f};
// fade1: how far the rifle scope's glass is faded in (0-1, linear in time; drawn eased), and when last stepped.
// While it is above 0 after the scope stopped being wanted, the picture keeps being drawn (ScopeRifleAutoNow) so it
// can fade out instead of vanishing.
static float g_fade;
static volatile DWORD g_fadeT;
volatile float g_eyeNow[3];  // autoscope2: last check: cm behind the glass, cm off its axis, deg off the barrel
bool g_rifleShow;  // menu only: the glass shows without aiming, to line it up
// autoscope1: [scope.<gun>] RifleAuto (default 1): the picture comes on with the eyepiece at the eye, no trigger needed;
// AutoZoomDeg: the game's zoom learnt from trigger-held scoping (used without the trigger). g_atEye: last left eye's check.
bool g_rifleAuto = true;
// steadyonly1 (DrGonzaga 22 Nov: "hold left trigger to scope is redundant ... make left trigger extra steadying only"):
// per gun TriggerSteadyOnly (1): on an auto rifle scope the left trigger is kept from the game (no game zoom) and only
// steadies the scope; the zoom is AutoZoomDeg (slider) times the lens zoom (A / X).
bool g_ltSteady = true;
float g_autoFov = 20.0f * 0.0174533f;
bool g_autoFovSave;
volatile bool g_atEye;
volatile DWORD g_atEyeT;
float g_retAlpha = 0.9f;  // reticle opacity (Aim page)   // extra zoom: lens shows 1/zoom of the scope image (Aim page)

bool g_failed;
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
ID3D11VertexShader *g_vs;
ID3D11PixelShader *g_ps;
ID3D11Buffer *g_cb;
ID3D11RasterizerState *g_rs;
ID3D11DepthStencilState *g_ds;
ID3D11BlendState *g_bs, *g_bsFade;
ID3D11SamplerState *g_smp;

IDXGISwapChain *g_sc;
ID3D11Texture2D *g_bb;
ID3D11RenderTargetView *g_rtv;  // ours, on the back buffer
ID3D11Texture2D *g_tex;         // copy of the last scope image
ID3D11ShaderResourceView *g_srv;
UINT g_w, g_h;
volatile DWORD g_capturedAt;    // when the copy was made
LONG g_drawn, g_copies;

void Release() {
    if (g_srv) g_srv->Release(), g_srv = nullptr;
    if (g_tex) g_tex->Release(), g_tex = nullptr;
    if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
    if (g_bb) g_bb->Release(), g_bb = nullptr;
    g_sc = nullptr;
    g_capturedAt = 0;
}

bool SetupDevice(IDXGISwapChain *sc) {
    if (g_failed) return false;
    if (g_dev) return true;
    ID3D11Device *dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev))) return g_failed = true, false;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    bool ok = compile && SUCCEEDED(compile(kShader, sizeof kShader - 1, "scope", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) &&
              SUCCEEDED(compile(kShader, sizeof kShader - 1, "scope", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err));
    if (compile && !ok) {
        Log("scope: reticle shader failed (%s); plain lens instead", err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release(), err = nullptr;
        if (vsb) vsb->Release(), vsb = nullptr;
        if (psb) psb->Release(), psb = nullptr;
        ok = SUCCEEDED(compile(kShaderPlain, sizeof kShaderPlain - 1, "scope", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) &&
             SUCCEEDED(compile(kShaderPlain, sizeof kShaderPlain - 1, "scope", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err));
    }
    if (!ok) {
        Log("scope: shader compile failed; scope lens off: %s", err ? (const char *)err->GetBufferPointer() : "no d3dcompiler");
        if (err) err->Release();
        if (vsb) vsb->Release();
        dev->Release();
        return g_failed = true, false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(), psb->Release();
    D3D11_BUFFER_DESC bd = {sizeof(Cb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = FALSE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dev->CreateDepthStencilState(&dd, &g_ds);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bl, &g_bs);
    // fade1: the glass blended over the game's picture by the blend factor (no shader change).
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR, bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE, bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bl, &g_bsFade);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_smp);
    dev->GetImmediateContext(&g_ctx);
    g_dev = dev;
    Log("scope: ready");
    return g_vs && g_ps && g_cb && g_rs && g_ds && g_bs && g_smp;
}

bool SetupChain(IDXGISwapChain *sc) {
    if (sc == g_sc && g_tex) return true;
    Release();
    ID3D11Texture2D *bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb))) return false;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    D3D11_TEXTURE2D_DESC td = d;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Usage = D3D11_USAGE_DEFAULT, td.CPUAccessFlags = 0, td.MiscFlags = 0;
    td.MipLevels = 1, td.ArraySize = 1, td.SampleDesc.Count = 1, td.SampleDesc.Quality = 0;
    if (d.SampleDesc.Count != 1 || FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_tex)) ||
        FAILED(g_dev->CreateShaderResourceView(g_tex, nullptr, &g_srv)) ||
        FAILED(g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv))) {
        Log("scope: couldn't make the scope texture (%ux%u format %d)", d.Width, d.Height, (int)d.Format);
        bb->Release();
        Release();
        g_failed = true;
        return false;
    }
    g_bb = bb, g_sc = sc, g_w = d.Width, g_h = d.Height;
    Log("scope: scope texture %ux%u format %d", d.Width, d.Height, (int)d.Format);
    return true;
}

float Dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void Cross(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
void Norm(float *v) {
    float l = sqrtf(Dot(v, v));
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}

}  // namespace

float *ScopeSize() { return &g_size; }
float *ScopeZoom() { return &g_zoom; }
float *ScopeReticle() { return g_reticle; }
float *ScopeReticleOpacity() { return &g_retAlpha; }
float *ScopeReticleTurn() { return &g_retTurn; }
float *ScopeReticleGlow() { return &g_glow; }
float *ScopeDistortion() { return &g_distort; }
// nohud1 (DrGonzaga 8 Nov): the game's HUD (minimap, "hold to steady" prompt) showed
// at the lens edge when zoomed out. Our reticles are all drawn here, so the scope
// image drops the game's HUD passes (hud.cpp points them at no target).
static bool g_scopeNoHud = true;  // [aim] ScopeHideHud
bool *ScopeHideHud() { return &g_scopeNoHud; }
// zoom1 (DrGonzaga 6 Nov): A / B while scoped step the extra zoom through fixed steps.
static const float kZoomSteps[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};
void ScopeZoomStep(int dir) {
    const int n = sizeof kZoomSteps / sizeof kZoomSteps[0];
    float z = g_zoom;
    int i = 0;
    if (dir > 0) {
        while (i < n - 1 && kZoomSteps[i] <= z + 0.01f) ++i;
    } else {
        i = n - 1;
        while (i > 0 && kZoomSteps[i] >= z - 0.01f) --i;
    }
    if (fabsf(kZoomSteps[i] - z) < 0.01f) return;  // already at the end
    g_zoom = kZoomSteps[i];
    MenuSaveLater(ScopeSave);  // written off the input thread, menu closed or not
}
static float g_recoil = 0.4f;  // recoil1: degrees the scope kicks up per shot (per gun)
float *ScopeRecoil() { return &g_recoil; }
int *ScopeStyle() { return &g_style; }
bool *ScopeSquare() { return &g_square; }  // square1
float *ScopeScan() { return &g_scan; }
float *ScopeTint() { return &g_tint; }
float *ScopeCorner() { return &g_corner; }  // square2
float *ScopeCrt() { return &g_crt; }
float *ScopeFringe() { return &g_fringe; }
static bool g_matchLaser;  // match1 (DrGonzaga 12 Nov): lit scope reticles take the gun's laser colour ([aim] ScopeReticleMatchLaser)
bool *ScopeMatchLaser() { return &g_matchLaser; }

// scope7: scope settings per gun ([scope.<gun>] in fc3vr.ini). A gun with none
// saved starts from the settings in use, like the gun positions.
static const wchar_t *const kRifle[13] = {L"EyeRight", L"EyeUp", L"EyeForward", L"GlassSize", L"EyeBox", L"EyeDistance", L"EyeRange", L"PictureFill",
                                           L"AutoReach", L"AutoSideways", L"AutoAngleDeg", L"FadeIn", L"FadeOut"};
static void ScopeIni(bool save) {
    if (!g_gunName[0]) return;
    wchar_t path[MAX_PATH], sec[80], buf[32];
    IniPath(path);
    swprintf(sec, 80, L"scope.%ls", g_gunName);
    // scope10 (DrGonzaga 1 Nov): steadiness is per gun too.
    float *v[7] = {&g_size, &g_zoom, &g_reticle[0], &g_reticle[1], &g_retAlpha, EngineScopeSmoothing(), &g_retTurn};
    const wchar_t *k[7] = {L"Size", L"Zoom", L"Right", L"Up", L"Opacity", L"Steadiness", L"ReticleTurn"};
    if (save) {
        for (int i = 0; i < 7; ++i) swprintf(buf, 32, L"%.3f", *v[i]), WritePrivateProfileStringW(sec, k[i], buf, path);
        swprintf(buf, 32, L"%d", g_style), WritePrivateProfileStringW(sec, L"Style", buf, path);
        swprintf(buf, 32, L"%.3f", g_recoil), WritePrivateProfileStringW(sec, L"Recoil", buf, path);
        WritePrivateProfileStringW(sec, L"Square", g_square ? L"1" : L"0", path);  // square1
        swprintf(buf, 32, L"%.3f", g_scan), WritePrivateProfileStringW(sec, L"Scanlines", buf, path);
        swprintf(buf, 32, L"%.3f", g_tint), WritePrivateProfileStringW(sec, L"OrangeTint", buf, path);
        swprintf(buf, 32, L"%.3f", g_corner), WritePrivateProfileStringW(sec, L"Corner", buf, path);  // square2
        swprintf(buf, 32, L"%.3f", g_crt), WritePrivateProfileStringW(sec, L"CrtBend", buf, path);
        swprintf(buf, 32, L"%.3f", g_fringe), WritePrivateProfileStringW(sec, L"CrtFringe", buf, path);
        WritePrivateProfileStringW(sec, L"RifleScope", g_rifle ? L"1" : L"0", path);  // rifle1
        WritePrivateProfileStringW(sec, L"RifleAuto", g_rifleAuto ? L"1" : L"0", path);  // autoscope1
        WritePrivateProfileStringW(sec, L"TriggerSteadyOnly", g_ltSteady ? L"1" : L"0", path);  // steadyonly1
        swprintf(buf, 32, L"%.2f", g_autoFov * 57.29578f), WritePrivateProfileStringW(sec, L"AutoZoomDeg", buf, path);
        for (int i = 0; i < 13; ++i) swprintf(buf, 32, L"%.4f", g_rv[i]), WritePrivateProfileStringW(sec, kRifle[i], buf, path);
        return;
    }
    if (!GetPrivateProfileStringW(sec, L"Size", L"", buf, 32, path)) {
        g_rifle = false;  // rifle1: nor this (the eyepiece place is the last gun's, to start from)
        g_square = false, g_scan = g_tint = 0, g_corner = 0.12f, g_crt = 0, g_fringe = 0.5f;  // square1: these do not carry over to the next gun
        ScopeIni(true);
        Log("scope: %ls has no scope settings yet; starting from the current ones", g_gunName);
        return;
    }
    // dmg2 (DrGonzaga 2 Nov, scope "sticky" again): guns saved before scope10 have
    // no Steadiness / ReticleTurn, so they kept the previous gun's (e.g. the
    // R700's 3.11 carried over to the MP5). Missing ones now get their own
    // defaults (DrGonzaga's usual 0.51 steadiness, no reticle turn), saved once.
    static const float kDef[7] = {-1, -1, -1, -1, -1, 0.51f, 0.0f};
    bool filled = false;
    for (int i = 0; i < 7; ++i) {
        if (GetPrivateProfileStringW(sec, k[i], L"", buf, 32, path)) *v[i] = (float)_wtof(buf);
        else if (kDef[i] >= 0) {
            *v[i] = kDef[i], filled = true;
            swprintf(buf, 32, L"%.3f", *v[i]), WritePrivateProfileStringW(sec, k[i], buf, path);
        }
    }
    if (filled) Log("scope: %ls had no steadiness / reticle turn saved; gave it the defaults", g_gunName);
    g_style = GetPrivateProfileIntW(sec, L"Style", g_style, path);
    g_recoil = GetPrivateProfileStringW(sec, L"Recoil", L"", buf, 32, path) ? (float)_wtof(buf) : 0.4f;
    g_square = GetPrivateProfileIntW(sec, L"Square", 0, path) != 0;  // square1: a gun without them: round, plain
    g_scan = GetPrivateProfileStringW(sec, L"Scanlines", L"", buf, 32, path) ? (float)_wtof(buf) : 0;
    g_tint = GetPrivateProfileStringW(sec, L"OrangeTint", L"", buf, 32, path) ? (float)_wtof(buf) : 0;
    g_corner = GetPrivateProfileStringW(sec, L"Corner", L"", buf, 32, path) ? (float)_wtof(buf) : 0.12f;  // square2
    g_crt = GetPrivateProfileStringW(sec, L"CrtBend", L"", buf, 32, path) ? (float)_wtof(buf) : 0;
    g_fringe = GetPrivateProfileStringW(sec, L"CrtFringe", L"", buf, 32, path) ? (float)_wtof(buf) : 0.5f;
    g_rifle = GetPrivateProfileIntW(sec, L"RifleScope", 0, path) != 0;  // rifle1
    g_rifleAuto = GetPrivateProfileIntW(sec, L"RifleAuto", 1, path) != 0;  // autoscope1
    g_ltSteady = GetPrivateProfileIntW(sec, L"TriggerSteadyOnly", 1, path) != 0;  // steadyonly1
    {
        float d = GetPrivateProfileStringW(sec, L"AutoZoomDeg", L"", buf, 32, path) ? (float)_wtof(buf) : 20.0f;
        g_autoFov = (d < 1 ? 1 : d > 40 ? 40 : d) * 0.0174533f;
    }
    g_atEye = false;
    for (int i = 7; i < 13; ++i) g_rv[i] = kRvDef[i];  // rifle2 / autoscope2 / fade1: a gun saved before these keys gets the defaults
    g_fade = 0;
    for (int i = 0; i < 13; ++i)
        if (GetPrivateProfileStringW(sec, kRifle[i], L"", buf, 32, path)) g_rv[i] = (float)_wtof(buf);
    Log("scope: %ls scope settings loaded (lens %.0f deg, zoom %.1fx, style %d, steadiness %.2f)", g_gunName, g_size, g_zoom, g_style,
        *EngineScopeSmoothing());
}
void ScopeSave() { ScopeIni(true); }
bool ScopeRifleOn() { return g_rifle; }
bool *ScopeRifle() { return &g_rifle; }
float *ScopeRifleValue(int i) { return &g_rv[i < 0 || i > 12 ? 0 : i]; }
float ScopeRifleDefault(int i) { return kRvDef[i < 0 || i > 12 ? 0 : i]; }
bool ScopeRifleAtEye(float now[3]) {
    for (int i = 0; i < 3; ++i) now[i] = g_eyeNow[i];
    return ScopeRifleAutoNow();
}
bool *ScopeRifleShowGlass() { return &g_rifleShow; }
bool *ScopeRifleAuto() { return &g_rifleAuto; }
bool ScopeRifleAutoNow() {
    const DWORD now = GetTickCount();
    if (!g_rifle || now - g_fadeT > 200) return false;
    return (g_rifleAuto && g_atEye && now - g_atEyeT < 200) || g_fade > 0.0f;  // fade1: still fading out
}
// fade1: step the fade once a frame (left eye), towards 1 while the scope is wanted (at the eye, or the trigger held).
static void RifleFadeTick() {
    static LARGE_INTEGER s_last, s_freq;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    float dt = s_last.QuadPart ? (float)(t.QuadPart - s_last.QuadPart) / (float)s_freq.QuadPart : 0.0f;
    s_last = t;
    if (dt > 0.1f || GetTickCount() - g_fadeT > 200) dt = 0.1f;  // a pause or the first frame: no jump from a stale time
    const bool want = (g_rifleAuto && g_atEye) || EngineScopeEyeActive();
    const float len = want ? g_rv[11] : g_rv[12];
    if (len < 0.005f) g_fade = want ? 1.0f : 0.0f;
    else g_fade += (want ? dt : -dt) / len;
    g_fade = g_fade < 0 ? 0 : g_fade > 1 ? 1 : g_fade;
    g_fadeT = GetTickCount();
}
float ScopeAutoFov() { return g_autoFov; }
float *ScopeAutoFovRef() { return &g_autoFov; }  // steadyonly1: menu slider
bool *ScopeRifleSteadyOnly() { return &g_ltSteady; }
bool ScopeSteadyOnlyNow() { return g_rifle && g_rifleAuto && g_ltSteady; }
const wchar_t *ScopeGun() { return g_gunName; }
static void ScopeCheckGun() {
    const char *n = GunAttachName();
    if (!n || !n[0]) return;
    wchar_t w[64];
    MultiByteToWideChar(CP_UTF8, 0, n, -1, w, 64);
    if (wcscmp(w, g_gunName) == 0) return;
    wcscpy(g_gunName, w);
    ScopeIni(false);
}
void ScopeOnSwapChainGoing() { Release(); }
// pip1 (vrs.cpp): the part of the scope image the lens can show: a circle round the image's middle, this many pixels
// across (radius), from the lens shader's sampling (0.5 + sc * d, |d| <= 1 incl. the square lens's corners).
float ScopeUsedRadiusPx(UINT h) { return 0.5f * (float)h * EngineScopeDrawnZoom() / (g_zoom < 1 ? 1 : g_zoom); }  // narrow1

// This Present's image is the scope view (right eye while scoped): keep it.
bool ScopeIsScopeImage() {
    int eye;
    float q[4], p[3];
    return EngineScopePictureActive() && EngineLastEye(&eye, q, p) && eye == EngineScopeEye();  // pip1: eye 2 (autoscope1: or at the eye)
}

void ScopeCapture(IDXGISwapChain *sc) {
    if (!SetupDevice(sc) || !SetupChain(sc)) return;
    g_ctx->CopyResource(g_tex, g_bb);
    g_capturedAt = GetTickCount();
    ++g_copies;
}

// autoscope1: is the eyepiece at the eye? Left eye's Present, every frame with a rifle scope. On: eye behind the glass no
// farther than the eye distance + range + 6 cm, within 5 cm of its axis, the gun pointing within 40 deg of the head's
// forward; off again past + 10 cm / 8 cm (so it doesn't flicker at the edge). Also learns the game's zoom while the
// trigger is held (for scoping without it).
static void RifleEyeCheck() {
    // zoomkeep1 (DrGonzaga 22 Nov: zoomed with the trigger and A, then without the trigger "it didn't save the zoom"): the
    // game's zoom was learnt on every zoomed frame, so letting go of the trigger learnt the frames where the game was
    // zooming back out (a wide view). Now only with the trigger held and the zoom steady for 0.3 s.
    {
        static float s_f;
        static DWORD s_since;
        const float f = EngineScopeFov();
        if (!EngineScopeEyeActive() || !(f > 0.01f)) s_since = 0;
        else if (!s_since || fabsf(f - s_f) > 0.002f) s_f = f, s_since = GetTickCount();
        else if (GetTickCount() - s_since > 300 && fabsf(f - g_autoFov) > 0.005f) g_autoFov = f, g_autoFovSave = true;
    }
    static DWORD s_saveT;
    if (g_autoFovSave && GetTickCount() - s_saveT > 3000) {
        s_saveT = GetTickCount(), g_autoFovSave = false;
        ScopeIni(true);
        Log("scope: %ls: the game's scope zoom %.1f deg (lens zoom %.1fx) kept for scoping without the trigger", g_gunName, g_autoFov * 57.29578f, g_zoom);
    }
    if (!g_rifleAuto) return void(g_atEye = false);
    EngineView v;
    float hand[3], dir[3], raw[3], m[16];
    if (!LaserRay(v, hand, dir, raw) || !HeadtrackHand(1, m, nullptr)) return;
    const float *r = v.basis, *u = v.basis + 3, *f = v.basis + 6;
    float ax[3][3];
    for (int i = 0; i < 3; ++i) {
        ax[0][i] = r[i] * m[0] + u[i] * m[4] - f[i] * m[8];
        ax[2][i] = -(r[i] * m[2] + u[i] * m[6] - f[i] * m[10]);
        ax[1][i] = r[i] * m[1] + u[i] * m[5] - f[i] * m[9];
    }
    for (auto &a : ax) Norm(a);
    float C[3], E[3];
    for (int i = 0; i < 3; ++i) C[i] = hand[i] + ax[0][i] * g_rv[0] + ax[1][i] * g_rv[1] + ax[2][i] * g_rv[2], E[i] = v.pos[i] - C[i];
    const float relief = -Dot(E, ax[2]), lx = Dot(E, ax[0]), ly = Dot(E, ax[1]), lat = sqrtf(lx * lx + ly * ly);
    float hf[3] = {v.fwd[0], v.fwd[1], v.fwd[2]};
    Norm(hf);
    const float along = Dot(hf, ax[2]), far0 = g_rv[5] + g_rv[6];
    const bool was = g_atEye;
    // autoscope2: the turn-on points are per gun now (were fixed: reach 6 cm, sideways 5 cm, 40 deg).
    const float reach = g_rv[8] < 0 ? 0 : g_rv[8], side = g_rv[9] < 0.005f ? 0.005f : g_rv[9];
    const float ang = (g_rv[10] < 5 ? 5 : g_rv[10] > 89 ? 89 : g_rv[10]) * 0.0174533f;
    const float angOff = ang + 10 * 0.0174533f < 1.57f ? ang + 10 * 0.0174533f : 1.57f;
    const float reachOff = reach + (reach * 0.25f > 0.04f ? reach * 0.25f : 0.04f);  // generous1: grows with a long reach (gunstock)
    g_eyeNow[0] = relief * 100, g_eyeNow[1] = lat * 100, g_eyeNow[2] = acosf(along < -1 ? -1 : along > 1 ? 1 : along) * 57.29578f;
    const bool on = was ? relief > -0.02f && relief < far0 + reachOff && lat < side * 1.6f && along > cosf(angOff)
                        : relief > 0.0f && relief < far0 + reach && lat < side && along > cosf(ang);
    if (on != was) LogV("scope: rifle scope %s the eye (eye %.1f cm behind the glass, %.1f cm off its axis)", on ? "at" : "away from", relief * 100, lat * 100);
    g_atEye = on, g_atEyeT = GetTickCount();
}

// rifle1: the eyepiece glass on the gun as this eye sees it, and what the shader needs for the view through it.
static bool RifleLayout(const EngineView &v, const float hand0[3], float tx, float ty, bool glassOnly, Cb &cb) {
    float m[16];
    if (!HeadtrackHand(1, m, nullptr)) return false;
    // The controller frame (rows right, forward, up), as the sight window uses it (laser.cpp), turned with the kick.
    const float *r = v.basis, *u = v.basis + 3, *f = v.basis + 6;
    float F[9], R[9], bk[3], ax[3][3], hand[3];
    for (int i = 0; i < 3; ++i) {
        F[i] = r[i] * m[0] + u[i] * m[4] - f[i] * m[8];
        F[3 + i] = -(r[i] * m[2] + u[i] * m[6] - f[i] * m[10]);
        F[6 + i] = r[i] * m[1] + u[i] * m[5] - f[i] * m[9];
    }
    memcpy(hand, hand0, 12);
    const float *a[3] = {F, F + 6, F + 3};  // right, up, forward
    const bool kicked = GunKickNow(F, R, bk);
    for (int j = 0; j < 3; ++j) {
        if (kicked) GunKickRotate(R, a[j], ax[j]);
        else memcpy(ax[j], a[j], 12);
        Norm(ax[j]);
    }
    if (kicked) for (int i = 0; i < 3; ++i) hand[i] += bk[i];
    float C[3];
    for (int i = 0; i < 3; ++i) C[i] = hand[i] + ax[0][i] * g_rv[0] + ax[1][i] * g_rv[1] + ax[2][i] * g_rv[2];
    float nr[3];
    Cross(v.fwd, v.up, nr);
    Norm(nr);
    auto view = [&](const float *w, float *o) { o[0] = Dot(w, nr), o[1] = Dot(w, v.up), o[2] = Dot(w, v.fwd); };
    // The eye must be behind the glass (on the stock side).
    float E[3] = {v.pos[0] - C[0], v.pos[1] - C[1], v.pos[2] - C[2]};
    const float relief = -Dot(E, ax[2]);
    if (relief < 0.005f) return false;
    const float rad = (g_rv[3] > 0.005f ? g_rv[3] : 0.005f) * 0.5f;
    for (int k = 0; k < 4; ++k) {  // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
        float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? -1.0f : 1.0f, P[3], pv[3];
        for (int i = 0; i < 3; ++i) P[i] = C[i] + (ax[0][i] * sx + ax[1][i] * sy) * rad - v.pos[i];
        view(P, pv);
        if (pv[2] < 0.003f) return false;  // a corner at or behind the eye
        cb.corner[k][0] = pv[0] / tx, cb.corner[k][1] = pv[1] / ty, cb.corner[k][2] = 0, cb.corner[k][3] = pv[2];
    }
    // The picture's up: the world's up seen across the scope's axis (the scope camera has no roll).
    float up[3] = {0, 0, 1}, ru[3], rr[3];
    float k = Dot(up, ax[2]);
    for (int i = 0; i < 3; ++i) ru[i] = up[i] - ax[2][i] * k;
    if (Dot(ru, ru) < 1e-4f) memcpy(ru, ax[1], 12);  // straight up or down
    Norm(ru);
    Cross(ax[2], ru, rr);  // right = forward x up
    Norm(rr);
    float A[3], RR[3], RU[3];
    view(ax[2], A), view(rr, RR), view(ru, RU);
    // rifle2 (DrGonzaga 21 Nov: "have the shaders for lens distortion etc apply to the size of the lens in the rifle"): the
    // picture's edge is the glass's edge as seen from the best eye distance (x PictureFill), not "Scope lens size", so the
    // rim effects (distortion, square, rounded corners, CRT, fringe, darkening) land on the glass.
    const float fill = g_rv[7] < 0.3f ? 0.3f : g_rv[7] > 3 ? 3 : g_rv[7];
    const float th = rad / (g_rv[5] > 0.02f ? g_rv[5] : 0.02f) * fill;
    cb.rf[0] = 1, cb.rf[1] = tx, cb.rf[2] = ty;
    memcpy(cb.rA, A, 12), cb.rA[3] = 1.0f / th;
    memcpy(cb.rR, RR, 12), memcpy(cb.rU, RU, 12);
    // Eye box: off the axis by more than EyeBox, the tube's shadow comes in from the far side (move towards it to clear
    // it); nearer or farther than EyeDistance +- EyeRange, the field closes in.
    const float box = g_rv[4] > 0.002f ? g_rv[4] : 0.002f, ed = g_rv[5], rng = g_rv[6] > 0.005f ? g_rv[6] : 0.005f;
    const float lx = Dot(E, rr), ly = Dot(E, ru), off = fabsf(relief - ed) - rng;
    cb.rB[0] = lx / box * 0.6f, cb.rB[1] = -ly / box * 0.6f;
    cb.rB[2] = glassOnly ? -1.0f : 1.6f - (off > 0 ? off / rng : 0.0f);
    cb.rB[3] = 0.25f;
    // The reticle turns with the gun (it is in the scope): the gun's up across the picture.
    {
        float gx = Dot(ax[1], rr), gy = Dot(ax[1], ru), l = sqrtf(gx * gx + gy * gy);
        cb.rot[0] = l > 0.2f ? gx / l : 0, cb.rot[1] = l > 0.2f ? gy / l : 1, cb.rot[2] = g_glow;
        // squareturn1 (DrGonzaga 22 Nov: the square eyebox is "a bit canted"): "Scope reticle turn" turns the glass, the
        // picture's square, the eye box and the reticle together on the rifle (clockwise), as it does the floating lens.
        const float a = g_retTurn * 0.0174533f, c = cosf(a), sn = sinf(a), ux = cb.rot[0], uy = cb.rot[1];
        cb.rot[0] = ux * c + uy * sn, cb.rot[1] = -ux * sn + uy * c;
        cb.rf[3] = a;
    }
    static DWORD s_log;
    if (g_verboseLog && GetTickCount() - s_log > 5000)
        s_log = GetTickCount(), Log("scope: rifle scope: eye %.1f cm behind the glass, %.1f cm right, %.1f cm up of its axis%s",
                                    relief * 100, lx * 100, ly * 100, glassOnly ? " (glass only)" : "");
    return true;
}

// Left eye's Present while scoped: draw the lens into its image.
void ScopeDraw(IDXGISwapChain *sc) {
    ScopeCheckGun();
    StockTick();  // ads1
    {  // scopewarm1 (bd62 log: the first zoom of a session stalled 0.2 s building the lens): built at start instead
        static bool s_warm;
        if (!s_warm) s_warm = true, (void)(SetupDevice(sc) && SetupChain(sc));
    }
    int eye = -1;
    float q[4], p[3];
    if (!EngineLastEye(&eye, q, p)) return;
    if (eye == 0 && g_rifle) RifleEyeCheck(), RifleFadeTick();  // autoscope1: is the eyepiece at the eye? fade1
    const bool active = EngineScopePictureActive() && (eye == 0 || (eye == 1 && EngineScopeEye() == 2));  // pip1: both eyes
    // rifle1: "Show the glass" (menu): the eyepiece shows (black) without aiming, to line it up with the gun's scope.
    const bool glassOnly = !active && g_rifle && g_rifleShow && (eye == 0 || eye == 1) && !EngineFlat() && EngineOnFoot();
    if (!active && !glassOnly) return;
    if (!g_tex || sc != g_sc) return;
    if (active && (!g_capturedAt || GetTickCount() - g_capturedAt > 100)) return;
    // fade1: the rifle glass fades in and out (eased); the floating lens and "show the glass" stay solid.
    float fadeA = 1.0f;
    if (active && g_rifle) {
        fadeA = g_fade * g_fade * (3 - 2 * g_fade);
        if (fadeA < 0.004f) return;
    }
    EngineView v;
    float tx, ty, hand[3], dir[3], raw[3];
    if (!LaserRay(v, hand, dir, raw) || !XrGetHeadsetFov(&tx, &ty)) return;
    Cb cb = {};
    if (g_rifle) {
        if (!RifleLayout(v, hand, tx, ty, glassOnly, cb)) return;
    } else {
        // Lens centre: towards the aim point, 100 m along the beam; a billboard 1 m away.
        float c[3];
        for (int i = 0; i < 3; ++i) c[i] = hand[i] + dir[i] * 100.0f - v.pos[i];
        Norm(c);
        float nr[3];
        Cross(v.fwd, v.up, nr);
        Norm(nr);
        float vc[3] = {Dot(c, nr), Dot(c, v.up), Dot(c, v.fwd)};  // view space: x right, y up, z ahead
        if (vc[2] < 0.2f) return;
        // scope11 (DrGonzaga 2 Nov): the scope image is level with the world (the
        // scope camera has no roll), but the lens was laid out with the head's up,
        // so tilting the head tilted the zoomed view against the world around it.
        // Lay the lens out with the world's up seen from the eye instead (a round
        // lens looks the same turned; image and reticle stay level).
        float vy[3] = {nr[2], v.up[2], v.fwd[2]};  // world up (+z) in view space
        {
            float l = sqrtf(vy[0] * vy[0] + vy[1] * vy[1]);
            if (l < 0.2f) vy[0] = 0, vy[1] = 1, vy[2] = 0;  // looking almost straight up or down
        }
        float lr[3], lu[3];
        Cross(vy, vc, lr);  // lens right, then up, facing the eye
        Norm(lr);
        Cross(vc, lr, lu);
        Norm(lu);
        float rad = tanf(g_size * 0.0174533f);
        for (int k = 0; k < 4; ++k) {  // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
            float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? -1.0f : 1.0f;
            float x = vc[0] + (lr[0] * sx + lu[0] * sy) * rad, y = vc[1] + (lr[1] * sx + lu[1] * sy) * rad,
                  z = vc[2] + (lr[2] * sx + lu[2] * sy) * rad;
            cb.corner[k][0] = x / tx, cb.corner[k][1] = y / ty, cb.corner[k][2] = 0, cb.corner[k][3] = z;
        }
        // scope12 (DrGonzaga 2 Nov): the reticle cants with the gun. The gun's up (as
        // we place it, so per-gun angle settings are included) seen on the lens.
        cb.rot[0] = 0, cb.rot[1] = 1, cb.rot[2] = g_glow;
        {
            float gm[16];
            if (GunAttachPose(gm)) {
                const float *gu = gm + 8;
                float g[3] = {Dot(gu, nr), Dot(gu, v.up), Dot(gu, v.fwd)};  // view space
                float gx = Dot(g, lr), gy = Dot(g, lu), l = sqrtf(gx * gx + gy * gy);
                if (l > 0.2f) cb.rot[0] = gx / l, cb.rot[1] = gy / l;
            }
            // Per-gun correction (each gun model's own axes differ), clockwise.
            float a = g_retTurn * 0.0174533f, c = cosf(a), sn = sinf(a), ux = cb.rot[0], uy = cb.rot[1];
            cb.rot[0] = ux * c + uy * sn, cb.rot[1] = -ux * sn + uy * c;
        }
    }
        cb.sty[0] = (float)g_style;
    static const float kLit[12][3] = {{0, 0, 0}, {0.15f, 0.45f, 1.0f}, {0.30f, 0.80f, 0.15f}, {0, 0, 0},
                                     {1.0f, 0.55f, 0.08f}, {0.25f, 0.95f, 0.30f}, {0.95f, 0.08f, 0.14f}, {0.15f, 0.85f, 1.0f},
                                     {1.0f, 0.10f, 0.80f}, {0.10f, 0.90f, 1.0f}, {0.45f, 1.0f, 0.20f}, {1.0f, 0.25f, 0.35f}};  // future1
    {  // reticle colour (blue dot, green prism; ret3 amber, green, ruby, cyan)
        const float *k = kLit[g_style < 0 || g_style > 11 ? 0 : g_style];
        static const float kLaser[4][3] = {{0.95f, 0.08f, 0.22f}, {0.15f, 0.95f, 0.25f}, {0.20f, 0.50f, 1.00f}, {1.00f, 0.50f, 0.08f}};
        if (g_matchLaser && k[0] + k[1] + k[2] > 0) k = kLaser[*LaserColour() & 3];  // the unlit (black) styles stay black
        cb.sty[1] = k[0], cb.sty[2] = k[1], cb.sty[3] = k[2];
    }
    cb.rot[3] = g_distort;  // lens1
    cb.fx[0] = g_square ? 1.f : 0.f, cb.fx[1] = g_scan, cb.fx[2] = g_tint, cb.fx[3] = (float)(GetTickCount() % 600000) * 0.001f;  // square1
    cb.fx2[0] = g_corner, cb.fx2[1] = g_crt, cb.fx2[2] = g_fringe, cb.fx2[3] = 0;  // square2
    cb.opt[0] = EngineScopeDrawnZoom() / (g_zoom < 1 ? 1 : g_zoom);  // narrow1: the picture already holds 1 / (drawn zoom) of the game's view
    cb.opt[1] = (float)g_w / g_h;
    cb.opt[2] = g_reticle[0], cb.opt[3] = -g_reticle[1];  // lens y runs down
    cb.col[0] = 0.80f, cb.col[1] = 0.04f, cb.col[2] = 0.20f, cb.col[3] = g_retAlpha;  // the beam's ruby red

    ID3D11DeviceContext *ctx = g_ctx;
    HeadtrackOwnDraw(true);
    D3DState game(ctx, 1, 1);  // state1: the game's pipeline state, put back by Restore() below

    ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)g_w, (float)g_h, 0, 1};
    EngineEyeViewport(&vp.TopLeftX, &vp.TopLeftY, &vp.Width, &vp.Height);  // oc1
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs);
    ctx->OMSetDepthStencilState(g_ds, 0);
    float bf[4] = {0, 0, 0, 0};
    if (fadeA < 0.999f && g_bsFade) {  // fade1
        bf[0] = bf[1] = bf[2] = bf[3] = fadeA;
        ctx->OMSetBlendState(g_bsFade, bf, 0xffffffff);
    } else ctx->OMSetBlendState(g_bs, bf, 0xffffffff);
    ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetShaderResources(0, 1, &g_srv);
    ctx->PSSetSamplers(0, 1, &g_smp);
    ctx->Draw(4, 0);
    ID3D11ShaderResourceView *none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);

    game.Restore();
    HeadtrackOwnDraw(false);
    ++g_drawn;
    static DWORD s_log;
    if (g_verboseLog && GetTickCount() - s_log > 5000)  // quiet1: detailed log only
        s_log = GetTickCount(),
        Log("scope: lens drawn (game zoom %.1f deg, lens %.0f deg wide = %.1fx, %ld copies)", EngineScopeFov() * 57.2958f,
            g_size * 2, g_size * 2 * (g_zoom < 1 ? 1 : g_zoom) / (EngineScopeFov() * 57.2958f + 1e-3f), g_copies);
}
