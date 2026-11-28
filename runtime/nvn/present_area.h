// Downscaling in the present pass: when the game renders larger than its place in the window (Video tab "Resolution"
// 2x-8x, or Dynamic above 1:1), each window pixel averages every game pixel it covers, weighted by the part it covers
// (fractional ratios included), in linear light (thin bright lines keep their brightness). One bilinear tap there
// kept 1 game pixel of 9 at 3:1 and aliased like native resolution. At 1:1 or below: the single tap as before (the
// upscalers run before the present and hand it a window-sized image).
// Shared by the Metal (metal.m present_src) and Vulkan (vulkan/shaders.c present.frag) present shaders; pf.y / pf.z
// carry the ratio (game pixels per window pixel, 0: off), uv / the crop rect are in the texture's uv.
#pragma once
#include <stdlib.h>

// pf[1..2] for the present shader: sw x sh game pixels shown in vw x vh window pixels. LARECOMP_PRESENT_AREA=0 (A/B
// tests): the single tap always.
static inline void present_area_params(float pf[4], double sw, double sh, double vw, double vh) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_PRESENT_AREA") || atoi(getenv("LARECOMP_PRESENT_AREA"));
    pf[1] = on && vw > 0 ? (float)(sw / vw) : 0;
    pf[2] = on && vh > 0 ? (float)(sh / vh) : 0;
}

// V2 / V3 / INT: the language's types; FETCH: texel (x, y); SIZE: the texture's size; DEC(q) / ENC(a): sRGB decode /
// encode of a V3; SAMPLE: the single tap; UV, CR: the uv and the shown rect (x0, y0, x1, y1). Sets `c`.
#define PRESENT_AREA_SRC(V2, V3, INT, FETCH, SIZE, DEC, ENC, SAMPLE, UV, CR)                                           \
    "  if (max(pf.y, pf.z) > 1.01) {\n"                                                                                \
    "    " V2 " sz = " SIZE ", r = min(max(pf.yz, " V2 "(1.0)), " V2 "(16.0));\n"                                      \
    "    " V2 " p = " UV " * sz, lo = max(p - r * 0.5, " CR ".xy * sz), hi = min(p + r * 0.5, " CR ".zw * sz);\n"     \
    "    " V3 " acc = " V3 "(0.0); float ws = 0.0;\n"                                                                  \
    "    for (" INT " y = " INT "(floor(lo.y)); float(y) < hi.y; y++) {\n"                                             \
    "      float wy = min(hi.y, float(y) + 1.0) - max(lo.y, float(y));\n"                                              \
    "      for (" INT " x = " INT "(floor(lo.x)); float(x) < hi.x; x++) {\n"                                           \
    "        float w = wy * (min(hi.x, float(x) + 1.0) - max(lo.x, float(x)));\n"                                      \
    "        " V3 " q = " FETCH ".rgb;\n"                                                                              \
    "        acc += w * " DEC ";\n"                                                                                    \
    "        ws += w;\n"                                                                                               \
    "      }\n"                                                                                                        \
    "    }\n"                                                                                                          \
    "    " V3 " a = acc / max(ws, 1e-6);\n"                                                                            \
    "    c = " ENC ";\n"                                                                                               \
    "  } else c = " SAMPLE ".rgb;\n"
