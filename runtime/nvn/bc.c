// BC1-BC5 block decoding (bc.h), following the block layouts and interpolation in the Vulkan spec's "Compressed
// Image Formats" (S3TC and RGTC) chapters.
#include "bc.h"

#include <stdbool.h>
#include <string.h>

// n / d rounded to the nearest (the spec interpolates in floating point; d > 0, n may be negative)
static int rdiv(int n, int d) { return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d); }

static void rgb565(uint16_t c, int out[3]) {
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    out[0] = r << 3 | r >> 2, out[1] = g << 2 | g >> 4, out[2] = b << 3 | b >> 2;
}

// Colour block (8 bytes) -> 16 texels. three_ok: BC1, where c0 <= c1 selects three colours and transparent black;
// in BC2 / BC3 the colour block always has four.
static void colour_block(const uint8_t *b, bool three_ok, uint8_t px[16][4]) {
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    int e[4][4];
    rgb565(c0, e[0]), rgb565(c1, e[1]);
    e[0][3] = e[1][3] = e[2][3] = e[3][3] = 255;
    if (c0 > c1 || !three_ok) {
        for (int i = 0; i < 3; i++) e[2][i] = rdiv(2 * e[0][i] + e[1][i], 3), e[3][i] = rdiv(e[0][i] + 2 * e[1][i], 3);
    } else {
        for (int i = 0; i < 3; i++) e[2][i] = rdiv(e[0][i] + e[1][i], 2), e[3][i] = 0;
        e[3][3] = 0;
    }
    uint32_t idx = (uint32_t)(b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24);
    for (int t = 0; t < 16; t++) {
        const int *c = e[(idx >> (2 * t)) & 3];
        px[t][0] = (uint8_t)c[0], px[t][1] = (uint8_t)c[1], px[t][2] = (uint8_t)c[2], px[t][3] = (uint8_t)c[3];
    }
}

// One-channel block (8 bytes, BC4 / BC3 alpha / each half of BC5) -> 16 values (signed ones as their two's complement
// byte when snorm)
static void channel_block(const uint8_t *b, bool snorm, uint8_t out[16]) {
    int v[8];
    int r0 = snorm ? (int8_t)b[0] : b[0], r1 = snorm ? (int8_t)b[1] : b[1];
    if (snorm) { if (r0 < -127) r0 = -127; if (r1 < -127) r1 = -127; }  // (-128 reads as -1.0, as -127)
    v[0] = r0, v[1] = r1;
    if (r0 > r1) {
        for (int i = 1; i < 7; i++) v[i + 1] = rdiv((7 - i) * r0 + i * r1, 7);
    } else {
        for (int i = 1; i < 5; i++) v[i + 1] = rdiv((5 - i) * r0 + i * r1, 5);
        v[6] = snorm ? -127 : 0, v[7] = snorm ? 127 : 255;
    }
    uint64_t idx = 0;
    for (int i = 0; i < 6; i++) idx |= (uint64_t)b[2 + i] << (8 * i);
    for (int t = 0; t < 16; t++) out[t] = (uint8_t)v[(idx >> (3 * t)) & 7];
}

static void block(int kind, const uint8_t *b, uint8_t px[16][4]) {
    uint8_t a[16], g[16];
    bool snorm = kind == BC4_SNORM || kind == BC5_SNORM;
    uint8_t zero = 0, one = snorm ? 127 : 255;
    switch (kind) {
    case BC1_RGB:
        colour_block(b, true, px);
        for (int t = 0; t < 16; t++) px[t][3] = 255;  // (no alpha: the transparent entry is opaque black)
        break;
    case BC1_RGBA: colour_block(b, true, px); break;
    case BC2:
        colour_block(b + 8, false, px);
        for (int t = 0; t < 16; t++) { int n = (b[t / 2] >> (4 * (t & 1))) & 15; px[t][3] = (uint8_t)(n << 4 | n); }
        break;
    case BC3:
        colour_block(b + 8, false, px);
        channel_block(b, false, a);
        for (int t = 0; t < 16; t++) px[t][3] = a[t];
        break;
    case BC4_UNORM: case BC4_SNORM:
        channel_block(b, snorm, a);
        for (int t = 0; t < 16; t++) px[t][0] = a[t], px[t][1] = px[t][2] = zero, px[t][3] = one;
        break;
    case BC5_UNORM: case BC5_SNORM:
        channel_block(b, snorm, a);
        channel_block(b + 8, snorm, g);
        for (int t = 0; t < 16; t++) px[t][0] = a[t], px[t][1] = g[t], px[t][2] = zero, px[t][3] = one;
        break;
    default: memset(px, 0, 64); break;
    }
}

void bc_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *rgba_out) {
    uint32_t wb = (width + 3) / 4, hb = (height + 3) / 4, bytes = bc_block_bytes(kind);
    uint8_t px[16][4];
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            block(kind, blocks + ((uint64_t)by * wb + bx) * bytes, px);
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; x++)
                    memcpy(rgba_out + (((uint64_t)(by * 4 + y) * width) + bx * 4 + x) * 4, px[y * 4 + x], 4);
        }
}
