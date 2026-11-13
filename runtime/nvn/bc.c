// BC1-BC7 block decoding (bc.h), following the block layouts and interpolation in the Khronos Data Format
// Specification's S3TC, RGTC and BPTC chapters (which the Vulkan spec's "Compressed Image Formats" points to).
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

// ---- BPTC (BC6H, BC7) ----

// Partitions: the subset of each texel (row-major), for two and three subsets
static const char part2[64][17] = {
    "0011001100110011", "0001000100010001", "0111011101110111", "0001001100110111", "0000000100010011", "0011011101111111",
    "0001001101111111", "0000000100110111", "0000000000010011", "0011011111111111", "0000000101111111", "0000000000010111",
    "0001011111111111", "0000000011111111", "0000111111111111", "0000000000001111", "0000100011101111", "0111000100000000",
    "0000000010001110", "0111001100010000", "0011000100000000", "0000100011001110", "0000000010001100", "0111001100110001",
    "0011000100010000", "0000100010001100", "0110011001100110", "0011011001101100", "0001011111101000", "0000111111110000",
    "0111000110001110", "0011100110011100", "0101010101010101", "0000111100001111", "0101101001011010", "0011001111001100",
    "0011110000111100", "0101010110101010", "0110100101101001", "0101101010100101", "0111001111001110", "0001001111001000",
    "0011001001001100", "0011101111011100", "0110100110010110", "0011110011000011", "0110011010011001", "0000011001100000",
    "0100111001000000", "0010011100100000", "0000001001110010", "0000010011100100", "0110110010010011", "0011011011001001",
    "0110001110011100", "0011100111000110", "0110110011001001", "0110001100111001", "0111111010000001", "0001100011100111",
    "0000111100110011", "0011001111110000", "0010001011101110", "0100010001110111"};
static const char part3[64][17] = {
    "0011001102212222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122", "0011001100220022",
    "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222", "0000111122222222", "0012001200120012",
    "0112011201120112", "0122012201220122", "0011011211221222", "0011200122002220", "0001001101121122", "0111001120012200",
    "0000112211221122", "0022002200221111", "0111011102220222", "0001000122212221", "0000001101220122", "0000110022102210",
    "0122012200110000", "0012001211222222", "0110122112210110", "0000011012211221", "0022110211020022", "0110011020022222",
    "0011012201220011", "0000200022112221", "0000000211221222", "0222002200120011", "0011001200220222", "0120012001200120",
    "0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011", "0011112222000011", "0101010122222222",
    "0000000021212121", "0022112200221122", "0022001100220011", "0220122102201221", "0101222222220101", "0000212121212121",
    "0101010101012222", "0222011102220111", "0002111200021112", "0000211221122112", "0222011101110222", "0002111211120002",
    "0110011001102222", "0000000021122112", "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
    "0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220"};
// Anchor texels (their index has one bit less): subset 1 of two; subsets 1 and 2 of three. Subset 0's is texel 0.
static const uint8_t anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
static const uint8_t anchor3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
static const uint8_t anchor3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8};
static const uint8_t weights2[4] = {0, 21, 43, 64};
static const uint8_t weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const uint8_t weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

static int weight(int bits, int i) { return bits == 2 ? weights2[i] : bits == 3 ? weights3[i] : weights4[i]; }

// n bits at *pos (least significant first through the block's bytes)
static uint32_t bits(const uint8_t *b, unsigned *pos, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++, (*pos)++) v |= (uint32_t)(b[*pos >> 3] >> (*pos & 7) & 1) << i;
    return v;
}

// Texel t's subset and the size of its index (one bit less at the subset's anchor)
static int subset_of(int ns, int part, int t) { return ns == 2 ? part2[part][t] - '0' : ns == 3 ? part3[part][t] - '0' : 0; }
static bool is_anchor(int ns, int part, int t) {
    return t == 0 || (ns == 2 && t == anchor2[part]) || (ns == 3 && (t == anchor3a[part] || t == anchor3b[part]));
}

static const struct { uint8_t ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; } bc7_modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

static void bc7_block(const uint8_t *b, uint8_t px[16][4]) {
    int mode = 0;
    while (mode < 8 && !(b[0] >> mode & 1)) mode++;
    if (mode == 8) { memset(px, 0, 64); return; }  // (reserved: transparent black)
    int ns = bc7_modes[mode].ns, cb = bc7_modes[mode].cb, ab = bc7_modes[mode].ab;
    int ib = bc7_modes[mode].ib, ib2 = bc7_modes[mode].ib2;
    unsigned pos = mode + 1;
    int part = bits(b, &pos, bc7_modes[mode].pb), rot = bits(b, &pos, bc7_modes[mode].rb), isb = bits(b, &pos, bc7_modes[mode].isb);
    int e[6][4];  // endpoint 0 and 1 of subset s: e[2s], e[2s + 1]
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < ns * 2; i++) e[i][c] = c < 3 ? bits(b, &pos, cb) : ab ? bits(b, &pos, ab) : 255;
    if (bc7_modes[mode].epb || bc7_modes[mode].spb) {  // P-bits: the lowest bit of each endpoint, or of each subset's pair
        cb++, ab += ab > 0;
        int p[6];
        for (int i = 0; i < ns * 2; i++) p[i] = bc7_modes[mode].epb || !(i & 1) ? (int)bits(b, &pos, 1) : p[i - 1];
        for (int i = 0; i < ns * 2; i++)
            for (int c = 0; c < 4; c++)
                if (c < 3 || ab) e[i][c] = e[i][c] << 1 | p[i];
    }
    for (int i = 0; i < ns * 2; i++)
        for (int c = 0; c < 4; c++) {
            int n = c < 3 ? cb : ab;
            if (n) e[i][c] = (e[i][c] << (8 - n)) | (e[i][c] >> (2 * n - 8));
        }
    int idx[16], idx2[16];
    for (int t = 0; t < 16; t++) idx[t] = bits(b, &pos, ib - is_anchor(ns, part, t));
    if (ib2) for (int t = 0; t < 16; t++) idx2[t] = bits(b, &pos, ib2 - (t == 0));
    for (int t = 0; t < 16; t++) {
        int s = subset_of(ns, part, t), wc, wa;
        if (!ib2) wc = wa = weight(ib, idx[t]);
        else if (isb) wc = weight(ib2, idx2[t]), wa = weight(ib, idx[t]);  // (mode 4: the 3-bit indices for colour)
        else wc = weight(ib, idx[t]), wa = weight(ib2, idx2[t]);
        for (int c = 0; c < 4; c++) {
            int w = c < 3 ? wc : wa;
            px[t][c] = (uint8_t)(((64 - w) * e[2 * s][c] + w * e[2 * s + 1][c] + 32) >> 6);
        }
        if (rot) { uint8_t a = px[t][3]; px[t][3] = px[t][rot - 1], px[t][rot - 1] = a; }
    }
}

// BC6H endpoint fields: w x of the first region (its endpoints 0 and 1), y z of the second; r g b each
enum { RW, GW, BW, RX, GX, BX, RY, GY, BY, RZ, GZ, BZ };
// Bits [hi:lo] of a field, read from lo towards hi (modes 13 and 14 store their top bits reversed: hi < lo)
typedef struct { uint8_t f, hi, lo; } Field;
static const struct {
    uint8_t epb, dr, dg, db, regions, transformed;
    Field fields[24];
} bc6_modes[14] = {
    {10, 5, 5, 5, 2, 1, {{GY, 4, 4}, {BY, 4, 4}, {BZ, 4, 4}, {RW, 9, 0}, {GW, 9, 0}, {BW, 9, 0}, {RX, 4, 0}, {GZ, 4, 4}, {GY, 3, 0},
                         {GX, 4, 0}, {BZ, 0, 0}, {GZ, 3, 0}, {BX, 4, 0}, {BZ, 1, 1}, {BY, 3, 0}, {RY, 4, 0}, {BZ, 2, 2}, {RZ, 4, 0},
                         {BZ, 3, 3}}},
    {7, 6, 6, 6, 2, 1, {{GY, 5, 5}, {GZ, 4, 4}, {GZ, 5, 5}, {RW, 6, 0}, {BZ, 0, 0}, {BZ, 1, 1}, {BY, 4, 4}, {GW, 6, 0}, {BY, 5, 5},
                        {BZ, 2, 2}, {GY, 4, 4}, {BW, 6, 0}, {BZ, 3, 3}, {BZ, 5, 5}, {BZ, 4, 4}, {RX, 5, 0}, {GY, 3, 0}, {GX, 5, 0},
                        {GZ, 3, 0}, {BX, 5, 0}, {BY, 3, 0}, {RY, 5, 0}, {RZ, 5, 0}}},
    {11, 5, 4, 4, 2, 1, {{RW, 9, 0}, {GW, 9, 0}, {BW, 9, 0}, {RX, 4, 0}, {RW, 10, 10}, {GY, 3, 0}, {GX, 3, 0}, {GW, 10, 10},
                         {BZ, 0, 0}, {GZ, 3, 0}, {BX, 3, 0}, {BW, 10, 10}, {BZ, 1, 1}, {BY, 3, 0}, {RY, 4, 0}, {BZ, 2, 2}, {RZ, 4, 0},
                         {BZ, 3, 3}}},
    {11, 4, 5, 4, 2, 1, {{RW, 9, 0}, {GW, 9, 0}, {BW, 9, 0}, {RX, 3, 0}, {RW, 10, 10}, {GZ, 4, 4}, {GY, 3, 0}, {GX, 4, 0},
                         {GW, 10, 10}, {GZ, 3, 0}, {BX, 3, 0}, {BW, 10, 10}, {BZ, 1, 1}, {BY, 3, 0}, {RY, 3, 0}, {BZ, 0, 0},
                         {BZ, 2, 2}, {RZ, 3, 0}, {GY, 4, 4}, {BZ, 3, 3}}},
    {11, 4, 4, 5, 2, 1, {{RW, 9, 0}, {GW, 9, 0}, {BW, 9, 0}, {RX, 3, 0}, {RW, 10, 10}, {BY, 4, 4}, {GY, 3, 0}, {GX, 3, 0},
                         {GW, 10, 10}, {BZ, 0, 0}, {GZ, 3, 0}, {BX, 4, 0}, {BW, 10, 10}, {BY, 3, 0}, {RY, 3, 0}, {BZ, 1, 1},
                         {BZ, 2, 2}, {RZ, 3, 0}, {BZ, 4, 4}, {BZ, 3, 3}}},
    {9, 5, 5, 5, 2, 1, {{RW, 8, 0}, {BY, 4, 4}, {GW, 8, 0}, {GY, 4, 4}, {BW, 8, 0}, {BZ, 4, 4}, {RX, 4, 0}, {GZ, 4, 4}, {GY, 3, 0},
                        {GX, 4, 0}, {BZ, 0, 0}, {GZ, 3, 0}, {BX, 4, 0}, {BZ, 1, 1}, {BY, 3, 0}, {RY, 4, 0}, {BZ, 2, 2}, {RZ, 4, 0},
                        {BZ, 3, 3}}},
    {8, 6, 5, 5, 2, 1, {{RW, 7, 0}, {GZ, 4, 4}, {BY, 4, 4}, {GW, 7, 0}, {BZ, 2, 2}, {GY, 4, 4}, {BW, 7, 0}, {BZ, 3, 3}, {BZ, 4, 4},
                        {RX, 5, 0}, {GY, 3, 0}, {GX, 4, 0}, {BZ, 0, 0}, {GZ, 3, 0}, {BX, 4, 0}, {BZ, 1, 1}, {BY, 3, 0}, {RY, 5, 0},
                        {RZ, 5, 0}}},
    {8, 5, 6, 5, 2, 1, {{RW, 7, 0}, {BZ, 0, 0}, {BY, 4, 4}, {GW, 7, 0}, {GY, 5, 5}, {GY, 4, 4}, {BW, 7, 0}, {GZ, 5, 5}, {BZ, 4, 4},
                        {RX, 4, 0}, {GZ, 4, 4}, {GY, 3, 0}, {GX, 5, 0}, {GZ, 3, 0}, {BX, 4, 0}, {BZ, 1, 1}, {BY, 3, 0}, {RY, 4, 0},
                        {BZ, 2, 2}, {RZ, 4, 0}, {BZ, 3, 3}}},
    {8, 5, 5, 6, 2, 1, {{RW, 7, 0}, {BZ, 1, 1}, {BY, 4, 4}, {GW, 7, 0}, {BY, 5, 5}, {GY, 4, 4}, {BW, 7, 0}, {BZ, 5, 5}, {BZ, 4, 4},
                        {RX, 4, 0}, {GZ, 4, 4}, {GY, 3, 0}, {GX, 4, 0}, {BZ, 0, 0}, {GZ, 3, 0}, {BX, 5, 0}, {BY, 3, 0}, {RY, 4, 0},
                        {BZ, 2, 2}, {RZ, 4, 0}, {BZ, 3, 3}}},
    {6, 6, 6, 6, 2, 0, {{RW, 5, 0}, {GZ, 4, 4}, {BZ, 0, 0}, {BZ, 1, 1}, {BY, 4, 4}, {GW, 5, 0}, {GY, 5, 5}, {BY, 5, 5}, {BZ, 2, 2},
                        {GY, 4, 4}, {BW, 5, 0}, {GZ, 5, 5}, {BZ, 3, 3}, {BZ, 5, 5}, {BZ, 4, 4}, {RX, 5, 0}, {GY, 3, 0}, {GX, 5, 0},
                        {GZ, 3, 0}, {BX, 5, 0}, {BY, 3, 0}, {RY, 5, 0}, {RZ, 5, 0}}},
    {10, 10, 10, 10, 1, 0, {{RW, 9, 0}, {GW, 9, 0}, {BW, 9, 0}, {RX, 9, 0}, {GX, 9, 0}, {BX, 9, 0}}},
    {11, 9, 9, 9, 1, 1, {{RW, 9, 0}, {GW, 9, 0}, {BW, 9, 0}, {RX, 8, 0}, {RW, 10, 10}, {GX, 8, 0}, {GW, 10, 10}, {BX, 8, 0}, {BW, 10, 10}}},
    {12, 8, 8, 8, 1, 1, {{RW, 9, 0}, {GW, 9, 0}, {BW, 9, 0}, {RX, 7, 0}, {RW, 10, 11}, {GX, 7, 0}, {GW, 10, 11}, {BX, 7, 0}, {BW, 10, 11}}},
    {16, 4, 4, 4, 1, 1, {{RW, 9, 0}, {GW, 9, 0}, {BW, 9, 0}, {RX, 3, 0}, {RW, 10, 15}, {GX, 3, 0}, {GW, 10, 15}, {BX, 3, 0}, {BW, 10, 15}}},
};

static int sext(int v, int n) { return n < 32 && (v >> (n - 1) & 1) ? v - (1 << n) : v; }

// An endpoint value of n bits -> 16 bits (the spec's unquantize)
static int bc6_unquantize(int v, int n, bool sgn) {
    if (!sgn) {
        if (n >= 15 || v == 0) return v;
        if (v == (1 << n) - 1) return 0xFFFF;
        return ((v << 16) + 0x8000) >> n;
    }
    if (n >= 16) return v;
    int neg = v < 0, m = neg ? -v : v, u;
    if (m == 0) u = 0;
    else if (m >= (1 << (n - 1)) - 1) u = 0x7FFF;
    else u = ((m << 15) + 0x4000) >> (n - 1);
    return neg ? -u : u;
}

// An interpolated value -> its half-float bits (the spec's final scale by 31/64, or 31/32 signed)
static uint16_t bc6_half(int v, bool sgn) {
    if (!sgn) return (uint16_t)((v * 31) >> 6);
    if (v < 0) return (uint16_t)(0x8000 | (((-v) * 31) >> 5));
    return (uint16_t)((v * 31) >> 5);
}

static void bc6_block(const uint8_t *b, bool sgn, uint16_t px[16][4]) {
    static const int8_t code_mode[32] = {[2] = 2, [6] = 3, [10] = 4, [14] = 5, [18] = 6, [22] = 7, [26] = 8, [30] = 9,
                                         [3] = 10, [7] = 11, [11] = 12, [15] = 13, [19] = -1, [23] = -1, [27] = -1, [31] = -1};
    int mode = (b[0] & 3) < 2 ? (b[0] & 3) : code_mode[b[0] & 31];
    for (int t = 0; t < 16; t++) px[t][0] = px[t][1] = px[t][2] = 0, px[t][3] = 0x3C00;
    if (mode < 0) return;  // (reserved modes decode as black)
    unsigned pos = mode < 2 ? 2 : 5;
    int e[12] = {0};
    for (int i = 0; i < 24; i++) {
        const Field *f = &bc6_modes[mode].fields[i];
        if (f->f == RW && !f->hi && !f->lo) break;  // (end of the list)
        if (f->hi >= f->lo) for (int k = f->lo; k <= f->hi; k++) e[f->f] |= (int)bits(b, &pos, 1) << k;
        else for (int k = f->lo; k >= f->hi; k--) e[f->f] |= (int)bits(b, &pos, 1) << k;
    }
    int regions = bc6_modes[mode].regions, epb = bc6_modes[mode].epb;
    int part = regions == 2 ? (int)bits(b, &pos, 5) : 0;
    int dn[3] = {bc6_modes[mode].dr, bc6_modes[mode].dg, bc6_modes[mode].db};
    if (sgn) for (int c = 0; c < 3; c++) e[c] = sext(e[c], epb);
    for (int i = 3; i < regions * 6; i++) {
        int c = i % 3;
        if (sgn || bc6_modes[mode].transformed) e[i] = sext(e[i], dn[c]);
        if (bc6_modes[mode].transformed) {  // the other endpoints are deltas from w
            e[i] = (e[c] + e[i]) & ((1 << epb) - 1);
            if (sgn) e[i] = sext(e[i], epb);
        }
    }
    for (int i = 0; i < regions * 6; i++) e[i] = bc6_unquantize(e[i], epb, sgn);
    int ib = regions == 2 ? 3 : 4;
    for (int t = 0; t < 16; t++) {
        int s = subset_of(regions, part, t), w = weight(ib, (int)bits(b, &pos, ib - is_anchor(regions, part, t)));
        for (int c = 0; c < 3; c++) {
            int a = e[s * 6 + c], z = e[s * 6 + 3 + c];
            px[t][c] = bc6_half(((64 - w) * a + w * z + 32) >> 6, sgn);
        }
    }
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
    case BC7: bc7_block(b, px); break;
    default: memset(px, 0, 64); break;
    }
}

void bc_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *rgba_out) {
    uint32_t wb = (width + 3) / 4, hb = (height + 3) / 4, bytes = bc_block_bytes(kind), tb = bc_texel_bytes(kind);
    union { uint8_t u8[16][4]; uint16_t u16[16][4]; } px;
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t *b = blocks + ((uint64_t)by * wb + bx) * bytes;
            if (kind == BC6H_UFLOAT || kind == BC6H_SFLOAT) bc6_block(b, kind == BC6H_SFLOAT, px.u16);
            else block(kind, b, px.u8);
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; x++)
                    memcpy(rgba_out + (((uint64_t)(by * 4 + y) * width) + bx * 4 + x) * tb, (uint8_t *)&px + (y * 4 + x) * tb, tb);
        }
}
