"""runtime/nvn/bc.c decodes BC1-BC5 blocks as the Vulkan spec describes them (S3TC and RGTC). Drivers that don't list a
BC format get these decoded images instead (vulkan/textures.c; issue #693: Xclipse 940 has no BC4 / BC5)."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bc.h"
static uint8_t out[16 * 16 * 4];
static const uint8_t *px(uint32_t w, uint32_t x, uint32_t y) { return out + (y * w + x) * 4; }
static void eq(const uint8_t *p, int r, int g, int b, int a) {
    if (p[0] != (uint8_t)r || p[1] != (uint8_t)g || p[2] != (uint8_t)b || p[3] != (uint8_t)a) {
        printf("got %d %d %d %d, want %d %d %d %d\n", p[0], p[1], p[2], p[3], r, g, b, a);
        assert(0);
    }
}
// 3-bit indices for texels 0..15 -> the 6 index bytes of a BC4 block
static void idx3(uint8_t *b, const int *ix) {
    unsigned long long v = 0;
    for (int t = 0; t < 16; t++) v |= (unsigned long long)ix[t] << (3 * t);
    for (int i = 0; i < 6; i++) b[i] = (uint8_t)(v >> (8 * i));
}
int main(void) {
    // BC1, four colours: c0 red > c1 blue; indices 0 1 2 3 along each row (0xE4 = 3 2 1 0 from the top bits)
    uint8_t bc1[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
    bc_decode(BC1_RGBA, bc1, 4, 4, out);
    eq(px(4, 0, 0), 255, 0, 0, 255); eq(px(4, 1, 0), 0, 0, 255, 255);
    eq(px(4, 2, 0), 170, 0, 85, 255); eq(px(4, 3, 3), 85, 0, 170, 255);
    // BC1, three colours (c0 <= c1): index 2 the midpoint, 3 transparent black (opaque in BC1_RGB)
    uint8_t bc1t[8] = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4};
    bc_decode(BC1_RGBA, bc1t, 4, 4, out);
    eq(px(4, 2, 0), 128, 0, 128, 255); eq(px(4, 3, 0), 0, 0, 0, 0);
    bc_decode(BC1_RGB, bc1t, 4, 4, out);
    eq(px(4, 3, 0), 0, 0, 0, 255);
    // BC2: explicit 4-bit alpha (texel 0: 0x0, texel 1: 0xF, ...), colour block always four colours (here c0 <= c1)
    uint8_t bc2[16] = {0xF0, 0x84, 0, 0, 0, 0, 0, 0, 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4};
    bc_decode(BC2, bc2, 4, 4, out);
    eq(px(4, 0, 0), 0, 0, 255, 0); eq(px(4, 1, 0), 255, 0, 0, 255);
    eq(px(4, 2, 0), 85, 0, 170, 0x44); eq(px(4, 3, 0), 170, 0, 85, 0x88);
    // BC4 UNORM, eight values (r0 > r1)
    uint8_t bc4[8] = {255, 0};
    int ramp[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7};
    idx3(bc4 + 2, ramp);
    bc_decode(BC4_UNORM, bc4, 4, 4, out);
    int want8[8] = {255, 0, 219, 182, 146, 109, 73, 36};
    for (int t = 0; t < 8; t++) eq(px(4, t % 4, t / 4), want8[t], 0, 0, 255);
    // BC4 UNORM, six values and 0 / 255 (r0 <= r1)
    uint8_t bc4b[8] = {0, 255};
    idx3(bc4b + 2, ramp);
    bc_decode(BC4_UNORM, bc4b, 4, 4, out);
    int want6[8] = {0, 255, 51, 102, 153, 204, 0, 255};
    for (int t = 0; t < 8; t++) eq(px(4, t % 4, t / 4), want6[t], 0, 0, 255);
    // BC4 SNORM: -128 reads as -127; signed bytes out, alpha 127 (1.0)
    uint8_t bc4s[8] = {127, 0x80};
    idx3(bc4s + 2, ramp);
    bc_decode(BC4_SNORM, bc4s, 4, 4, out);
    eq(px(4, 0, 0), 127, 0, 0, 127); eq(px(4, 1, 0), (uint8_t)-127, 0, 0, 127); eq(px(4, 2, 0), 91, 0, 0, 127);
    uint8_t bc4s6[8] = {0xF6, 10};  // (-10 <= 10: six values, then -127 / 127)
    idx3(bc4s6 + 2, ramp);
    bc_decode(BC4_SNORM, bc4s6, 4, 4, out);
    eq(px(4, 2, 0), (uint8_t)-6, 0, 0, 127); eq(px(4, 2, 1), (uint8_t)-127, 0, 0, 127); eq(px(4, 3, 1), 127, 0, 0, 127);
    // BC5: R from the first half, G from the second; B 0, A 1 (the button glyph masks)
    uint8_t bc5[16] = {255, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0};
    idx3(bc5 + 2, ramp);
    idx3(bc5 + 10, ramp);
    bc_decode(BC5_UNORM, bc5, 4, 4, out);
    eq(px(4, 0, 0), 255, 0, 0, 255); eq(px(4, 1, 0), 0, 255, 0, 255); eq(px(4, 2, 0), 219, 51, 0, 255);
    // BC3: BC4-style alpha, then the colour block
    uint8_t bc3[16] = {255, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
    idx3(bc3 + 2, ramp);
    bc_decode(BC3, bc3, 4, 4, out);
    eq(px(4, 0, 0), 255, 0, 0, 255); eq(px(4, 1, 0), 0, 0, 255, 0); eq(px(4, 2, 0), 170, 0, 85, 219);
    // a 6x5 image: 2x2 blocks, the right and bottom ones cut; each block a different solid value
    uint8_t four[4 * 8];
    memset(four, 0, sizeof four);
    for (int k = 0; k < 4; k++) four[k * 8] = four[k * 8 + 1] = (uint8_t)(10 + k);  // (r0 == r1: all index 0)
    memset(out, 0xAB, sizeof out);
    bc_decode(BC4_UNORM, four, 6, 5, out);
    eq(px(6, 0, 0), 10, 0, 0, 255); eq(px(6, 5, 0), 11, 0, 0, 255); eq(px(6, 3, 4), 12, 0, 0, 255); eq(px(6, 5, 4), 13, 0, 0, 255);
    assert(out[6 * 5 * 4] == 0xAB);  // (nothing past the image)
    puts("PASS: BC1 (both modes), BC2, BC3, BC4 / BC5 UNORM and SNORM, partial blocks");
}
'''
with tempfile.TemporaryDirectory(prefix='bc-decode-') as temporary:
    temporary = Path(temporary)
    cfile = temporary / 'bc_test.c'; cfile.write_text(harness)
    exe = temporary / ('bc_test.exe' if os.name == 'nt' else 'bc_test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-I', str(root / 'runtime/nvn'),
                   str(cfile), str(root / 'runtime/nvn/bc.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=5)
