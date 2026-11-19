// SHA-512 (sha512.h), written from the standard (FIPS 180-4). The randomizer's own copy: Monocypher's (runtime/
// update_crypto_ed25519.c) is only in the desktops that update themselves, and the seeds are made everywhere.
#include "sha512.h"

#include <string.h>

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

static uint64_t ror(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void block(uint64_t h[8], const uint8_t *p) {
    uint64_t w[80];
    for (int t = 0; t < 16; t++) {
        w[t] = 0;
        for (int b = 0; b < 8; b++) w[t] = w[t] << 8 | p[8 * t + b];
    }
    for (int t = 16; t < 80; t++) {
        uint64_t s0 = ror(w[t - 15], 1) ^ ror(w[t - 15], 8) ^ (w[t - 15] >> 7);
        uint64_t s1 = ror(w[t - 2], 19) ^ ror(w[t - 2], 61) ^ (w[t - 2] >> 6);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 80; t++) {
        uint64_t t1 = hh + (ror(e, 14) ^ ror(e, 18) ^ ror(e, 41)) + ((e & f) ^ (~e & g)) + K512[t] + w[t];
        uint64_t t2 = (ror(a, 28) ^ ror(a, 34) ^ ror(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void rando_sha512(uint8_t out[64], const uint8_t *msg, size_t len) {
    uint64_t h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                     0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    size_t done = 0;
    for (; len - done >= 128; done += 128) block(h, msg + done);
    // the tail: the rest, 0x80, zeros, then the length in bits as 128 bits big-endian
    uint8_t tail[256] = {0};
    size_t rest = len - done;
    if (rest) memcpy(tail, msg + done, rest);
    tail[rest] = 0x80;
    size_t n = rest + 1 + 16 <= 128 ? 128 : 256;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[n - 1 - i] = (uint8_t)(bits >> (8 * i));
    tail[n - 9] = (uint8_t)((uint64_t)len >> 61);  // (the top bits of len * 8)
    block(h, tail);
    if (n == 256) block(h, tail + 128);
    for (int i = 0; i < 8; i++)
        for (int b = 0; b < 8; b++) out[8 * i + b] = (uint8_t)(h[i] >> (56 - 8 * b));
}
