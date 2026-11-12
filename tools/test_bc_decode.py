"""runtime/nvn/bc.c decodes BC1-BC7 blocks as the Khronos Data Format spec describes them (S3TC, RGTC, BPTC). Drivers
that don't list a BC format get these decoded images instead (vulkan/textures.c; issue #693: Xclipse 940 has no BC4,
BC5 or BC7)."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]

# BC7 (one block per mode) and BC6H (one per mode, unsigned and signed): random blocks with the texels an Apple M-series
# GPU decodes them to. BC7: all 16 texels as RGBA8. BC6H: texels 0, 5, 10 and 15 as RGB half-float bits.
bc7 = [
    ('05b6531d638f5c9c2165513b4797f64f', '3b6b61ff3b6b61ff545b7dffbd18efffcba152ff545b7dff71499bffa48323ffae5984ffae5984ffac8b3dffac8b3dffe7e721ffe7e721ffceadabffceadabff'),
    ('9efa37f576e0756dd5aaa30a2ebc779d', 'ebdbb7ff7e79b4ff9d4271ff7e79b4ff4e7ab7ff8d2464ffc777aeffebdbb7ffdf77adffbd818efff776abff9d4271ffbd818effc777aeffbd818eff7e79b4ff'),
    ('5441075dd7a01355bbf46b7f111369d9', '000829ff4e4c6aff000829ffefd6efff000829ff4e4c6aff000829ff4e4c6affd652ffff4e68baffadde29ffa187b8ffd652ffffefa5b5ffbab06fff4e68baff'),
    ('88f5e2f5b48d78eebd325c367e584c36', '9c7550ffe28832ffe2b3c3ffe2b3c3ff7a6c5effc07f40ff9c7550ffe2b3c3ff7a6c5effe28832ff7a6c5effe2b3c3ffc07f40ff9c7550ffe28832ff7a6c5eff'),
    ('50e1f0c1c09f04dc63bdc0b980576bd1', '18ff43a308ff63e7082463e7088063e729a3205c39e0001829ff205c39800018182443a308c163e7396200182962205c2943205c39c10018188043a3184343a3'),
    ('a0a4a108629cd7f8b9fed5d08b8e7193', '484d8d448766e7205d35ab385d66ab388766e720877ee7208735e7208766e720724dc92c7235c92c727ec92c5d4dab38487e8d447235c92c724dc92c8766e720'),
    ('40b887485622d85fefe04f5d89646a12', '94a657cc47c519c1e08894d847c519c13dc911bfb59971d154c023c3ab9d69d07faf46c989ab4ecbb59971d19ea25fce72b43cc79ea25fcec99182d4d68c8cd6'),
    ('80a1eaf1e85703092d2935ab1eb4c38c', '85b71952ef7d0c5551d3205151d32051564056a7564056a7707660a13c0c4dae85b7195251d32051bb99135485b71952564056a7707660a18aaa699a707660a1'),
]
bc6u = [
    ('4015575549d390577939b90ca49548ba', '14f3535515281376529c162313fd53d415ec13d2536f15fe'),
    ('a18899e09f2aa2f1fa7b2f8702a481b2', '4b1738d661213c3e36096c1b3b6d35e66a9b508e3dba58ca'),
    ('8220c68f0222caff0c5a8283d07d4511', '0fc5750151d40fc5750151d40fc5750c51c90fc5750151d4'),
    ('26775d29356f4547bdf82fe9c41353f7', '39e2689227f039c8686227f939f168ad27eb3a1868f527de'),
    ('0a542896eb4cf0affeab802bb968c8eb', '66b704df1bd266b9052b1bff66a8053c1c1f66dc05081bbd'),
    ('8ea39e3b397393601a4aded196c52c40', '459e4c62263f465b4bda265a459e4c62263f45614c8d2636'),
    ('b26b130519a502080c92271dd95c18ae', '2b5011e63e1f2de10bbb388d2de10bbb388d23310ed139a4'),
    ('d6a42bb1166e567440b8530bf99a1faf', '12a62a622ade118f32752c0c0bfd22ca2aa7135825db2da7'),
    ('faf3ae8594254f05d90e841def389c21', '4c4d2ccf20e4467a29e6250e531226821c56531226821c56'),
    ('3eed7a8c2fe9b27193ac371b1dc36a40', '4f515f7b147e28b05fc971e31f7c556e72764e3a574f1c65'),
    ('43863d2373e192d1d05d9d4a6ec26327', '061d4cf4309e05d32a2731e9060c44b630ec060c44b630ec'),
    ('87a8092bbbd68b4216188ca96a78a0db', '544a21561a30589023341cd4589023341cd45cd525121f79'),
    ('8bd53c46c631f538528029bfc259f96a', '33d46083188933f5604e18ce34ba5f1d1a5b34695f9b19b7'),
    ('cf113f8b1cb6fd46cc8b084ec0d52ea8', '19753d4408d819743d4508d919753d4408d819753d4408d8'),
]
bc6s = [
    ('406f45d13e66ce2d311dc35c6a867781', 'a093dab3a4ef9f98dc84a5a29f1edd67a5f99f98dc84a5a2'),
    ('81dfea0d67891e04ee81b04719333319', '88b8d4480c988e2bd78d0ec6aa05e84319ea9a28ad889458'),
    ('a2145c6e9e310b7937330547bb9cc944', 'e84ce5e5985de81fe649990ee81fe649990ee87fe6979905'),
    ('a601475921dbc8af5a94e958821bffd2', '01c54f75e74401195026e6ee00c04ebfe78f01044fcee715'),
    ('4a29abf8811674ea316529858c7debee', 'd41929791e2ad41929791dbcd41929791d53d4f729061fc7'),
    ('0ecbf61535db64dca13e44425e882d7f', '2b468909428a2d5d86f23fc12c178838417324008af741e9'),
    ('f20e5c5c6ace798566308aebced15a34', '3149c26b2fc5b621bc7c3402100bc08331225286c4532e68'),
    ('3642140c9693fc093dcf2408d4f7d8df', '0f0b236b0463091c178d818b0f042e9783830f042bdd85b1'),
    ('fac144e0651d1333233bd53a484264e2', '10a6f4ec93860000e63409340000e6340934023f889c08cb'),
    ('be18d8db865393e047a1167a0266f49f', '0953a881c01e82aa9113b4ad0953a881c01e9bf920649c84'),
    ('e3676deb7646e27555499dd027391753', 'a13dc56aa67c0aa8bf45b5a10aa8bf45b5a18f13c2dfacc0'),
    ('873c08bed74eac11db2454c055a7aaa7', '3928845182a03928845182a038838706820337948aef8120'),
    ('0bf481eced84c39a03191101936eee9f', '37d74dd18fef37d74dd18fef32994f4d8d2034a94eb78e3a'),
    ('6fe6eb74a61b7db3bad170aadbdad95c', '904579e567e2904479e467e3904379e367e4904579e567e2'),
]

def c_vectors():
    rows = []
    for kind, table in (('BC7', bc7), ('BC6H_UFLOAT', bc6u), ('BC6H_SFLOAT', bc6s)):
        for block, want in table:
            rows.append('{%s, "%s", "%s"}' % (kind, block, want))
    return ',\n    '.join(rows)

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
static const struct { int kind; const char *block, *want; } vectors[] = {
    VECTORS
};
static unsigned hex(const char *s, int n) { unsigned v = 0; for (int i = 0; i < n; i++) v = v << 4 | (unsigned)(s[i] <= '9' ? s[i] - '0' : s[i] - 'a' + 10); return v; }
static void bptc(void) {
    for (unsigned v = 0; v < sizeof vectors / sizeof *vectors; v++) {
        uint8_t block[16], got[16 * 8];
        for (int i = 0; i < 16; i++) block[i] = (uint8_t)hex(vectors[v].block + 2 * i, 2);
        bc_decode(vectors[v].kind, block, 4, 4, got);
        const char *w = vectors[v].want;
        if (vectors[v].kind == BC7) {
            for (int i = 0; i < 64; i++)
                if (got[i] != hex(w + 2 * i, 2)) { printf("BC7 vector %u (mode %d) texel %d channel %d: got %d want %u\n", v, v, i / 4, i % 4, got[i], hex(w + 2 * i, 2)); assert(0); }
        } else {
            for (int t = 0; t < 4; t++)
                for (int c = 0; c < 4; c++) {
                    uint16_t h;
                    memcpy(&h, got + (t * 5) * 8 + c * 2, 2);
                    unsigned want = c < 3 ? hex(w + (t * 3 + c) * 4, 4) : 0x3C00;  // (alpha 1.0)
                    if (h != want) { printf("BC6H vector %u texel %d channel %d: got %04x want %04x\n", v, t * 5, c, h, want); assert(0); }
                }
        }
    }
}
int main(void) {
    bptc();
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
    puts("PASS: BC1 (both modes), BC2, BC3, BC4 / BC5 UNORM and SNORM, BC6H (every mode, both signs), BC7 (every mode), partial blocks");
}
'''
with tempfile.TemporaryDirectory(prefix='bc-decode-') as temporary:
    temporary = Path(temporary)
    cfile = temporary / 'bc_test.c'; cfile.write_text(harness.replace('VECTORS', c_vectors()))
    exe = temporary / ('bc_test.exe' if os.name == 'nt' else 'bc_test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-I', str(root / 'runtime/nvn'),
                   str(cfile), str(root / 'runtime/nvn/bc.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=5)
