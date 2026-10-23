// Stress test of simd.h's fma emulation (SIMD_FMA without the FMA instruction) against libm's correctly rounded fmaf,
// in all four rounding modes: random operands, values built to land on float rounding midpoints after the double
// sum (the double-rounding cases), subnormal / overflow edges. Then a throughput comparison.
// Build: clang -O1 -march=x86-64-v2 -mavx -mf16c -Iruntime tools/test_fma_emu.c -o build/test_fma_emu -lm
#include <stdio.h>
#include <time.h>

#include "cpu.h"

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t bitsf(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static float rnd_float(void) {
    switch (rnd() % 6) {
    case 0: return fbits((uint32_t)rnd());                                        // anything (NaN, inf, subnormal)
    case 1: return fbits(((uint32_t)rnd() & 0x807fffff) | (uint32_t)(rnd() % 20 + 117) << 23);  // near 1
    case 2: return fbits((uint32_t)rnd() & 0x80ffffff);                          // subnormal / tiny
    case 3: return fbits(((uint32_t)rnd() & 0x807fffff) | (uint32_t)(rnd() % 4 + 250) << 23);  // near overflow
    case 4: return (float)(int)(rnd() % 4096) - 2048;                           // small integers
    default: return fbits(((uint32_t)rnd() & 0x80ffffff) | (uint32_t)(rnd() % 60 + 97) << 23);
    }
}

static int same(float a, float b) { return bitsf(a) == bitsf(b) || (a != a && b != b); }

int main(void) {
    static const int modes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    long fails = 0, n = 0, mids = 0;
    for (int m = 0; m < 4; m++) {
        fesetround(modes[m]);
        for (long i = 0; i < 4000000; i++) {
            float a = rnd_float(), b = rnd_float(), c;
            if (i & 1) {  // c chosen so a*b + c sits right at (or a hair off) a float midpoint
                double p = (double)a * (double)b;
                float hi = (float)p;
                double tail = p - (double)hi;
                c = (float)(-tail + (double)((int)(rnd() % 3) - 1) * (double)fbits(0x00000001u) * 0.5);
                if (rnd() & 1) c = fbits(bitsf(c) ^ 1);
            } else {
                c = rnd_float();
            }
            float want = fmaf(a, b, c);
            float got = simd_fmaf(a, b, c);
            vf32 va = {a, b, c, a}, vb = {b, a, a, c}, vc = {c, c, b, b};
            vf32 vg = simd_vfmaf(va, vb, vc);
            float vw[4] = {fmaf(a, b, c), fmaf(b, a, c), fmaf(c, a, b), fmaf(a, c, b)};
            int bad = !same(got, want);
            for (int k = 0; k < 4; k++) bad |= !same(vg[k], vw[k]);
            double s = (double)a * b + c;
            uint64_t u; memcpy(&u, &s, 8);
            mids += (u & 0x1fffffff) == 0x10000000;
            n++;
            if (bad && fails++ < 10)
                printf("mode %d: fma(%a, %a, %a) = %a, emulated %a\n", m, a, b, c, want, got);
        }
    }
    fesetround(FE_TONEAREST);
    printf("%ld cases (%ld on a double-rounding midpoint), %ld failing\n", n, mids, fails);

    // throughput: 4-lane vector fma, dependent chains
    enum { N = 1 << 24 };
    vf32 x = {1.0001f, 0.9999f, 1.5f, -0.75f}, y = {0.999f, 1.001f, 0.5f, 0.25f}, z = {1e-3f, -1e-3f, 0.1f, 0.2f};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) { x = simd_vfmaf(x, y, z); __asm__ volatile("" : "+x"(x)); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double emu = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / N;
    vf32 x2 = {1.0001f, 0.9999f, 1.5f, -0.75f};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) {
        x2 = (vf32){fmaf(x2[0], y[0], z[0]), fmaf(x2[1], y[1], z[1]), fmaf(x2[2], y[2], z[2]), fmaf(x2[3], y[3], z[3])};
        __asm__ volatile("" : "+x"(x2));
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double lib = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / N;
    printf("4-lane fma: emulated %.2f ns, libm calls %.2f ns (%g %g)\n", emu, lib, (double)x[0], (double)x2[0]);
    return fails != 0;
}
