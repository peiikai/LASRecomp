// Control-flow regression test for the Maxwell shader translator (runtime/nvn/maxwell/translate.c).
//
// Each case is a small fragment shader whose result depends on a branch. It is translated in one mode (MSL, GLSL
// with the pc state machine, or GLSL with structured control flow, the default on Adreno and Mali), the body of its
// main function is written into a C++ file with stand-ins for the few built-ins it uses, compiled and run on the CPU
// with several constant buffer values, and R0 (color target 0's red) is compared with what the shader should give.
// tools/test_translate_flow.sh drives it, once per mode (the translator reads the mode's switches once per process).
//
// "branch-phi" is the test shader from circerio/XC2-Recomp-Contributions (translator/branch-phi, MIT): a predicated
// SYNC that skips an assignment inside an SSY region. Their translator's branch elimination turned that assignment into
// an unconditional one. Re-encoded here from the instruction list in their make_repro.py; it contains no game data.
//
// usage: test_translate_flow msl|glsl|structured <out.cpp>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/maxwell/shader.h"

#define ONE 0x3f800000u
#define NOP 0x50b0000000070f00ull
#define RZ 255

static uint64_t mov32i(int rd, uint32_t v) { return 0x010ull << 52 | (uint64_t)v << 20 | 7ull << 16 | 0xfull << 12 | (uint64_t)rd; }
// FSETP.LE.AND Pd, PT, Ra, c[slot][word], PT
static uint64_t fsetp_le_c(int pd, int ra, int slot, int word) {
    return 0x4bbull << 52 | 3ull << 48 | 7ull << 39 | (uint64_t)slot << 34 | (uint64_t)word << 20 | 7ull << 16 | (uint64_t)ra << 8 |
           (uint64_t)pd << 3 | 7;
}
static uint64_t ssy(uint32_t at, uint32_t target) { return 0xe29ull << 52 | (uint64_t)((target - at - 8) & 0xffffff) << 20 | 7ull << 16; }
static uint64_t sync_(int pred, bool neg) { return 0x1e1full << 51 | (neg ? 1ull << 19 : 0) | (uint64_t)pred << 16 | 0xf; }
static uint64_t bra(uint32_t at, uint32_t target, int pred, bool neg) {
    return 0xe24ull << 52 | (uint64_t)((target - at - 8) & 0xffffff) << 20 | (neg ? 1ull << 19 : 0) | (uint64_t)pred << 16 | 0xf;
}
static uint64_t exit_(void) { return 0xe30ull << 52 | 7ull << 16 | 0xf; }

typedef struct {
    const char *name;
    uint64_t code[16];  // from code address 0; scheduling words (0x00, 0x20, ...) are 0, gaps NOP
    bool used[16];
} Case;

static void put(Case *c, uint32_t at, uint64_t w) { c->code[at / 8] = w, c->used[at / 8] = true; }

// R0..R3 = 0, 0, 0, 1; P0 = 0 <= c[3][0]; then the case's branch; R0 should end as c[3][0] >= 0 ? 1.0 : 0.0
static void prologue(Case *c) {
    put(c, 0x08, mov32i(0, 0));
    put(c, 0x10, mov32i(1, 0));
    put(c, 0x18, mov32i(2, 0));
    put(c, 0x28, mov32i(3, ONE));
    put(c, 0x30, fsetp_le_c(0, RZ, 3, 0));
}

static int cases(Case *cs) {
    int n = 0;
    {   // SSY L; @!P0 SYNC; MOV32I R0, 1.0; SYNC; L: EXIT
        Case *c = &cs[n++];
        *c = (Case){.name = "branch-phi (XC2 repro): @!P0 SYNC skips R0 = 1.0"};
        prologue(c);
        put(c, 0x38, ssy(0x38, 0x68));
        put(c, 0x48, sync_(0, true));
        put(c, 0x50, mov32i(0, ONE));
        put(c, 0x58, sync_(7, false));
        put(c, 0x68, exit_());
        put(c, 0x70, bra(0x70, 0x70, 7, false));
    }
    {   // the same with plain branches: @!P0 BRA L; MOV32I R0, 1.0; BRA L; L: EXIT
        Case *c = &cs[n++];
        *c = (Case){.name = "@!P0 BRA skips R0 = 1.0, then BRA to the same target"};
        prologue(c);
        put(c, 0x38, bra(0x38, 0x68, 0, true));
        put(c, 0x48, mov32i(0, ONE));
        put(c, 0x50, bra(0x50, 0x68, 7, false));
        put(c, 0x68, exit_());
        put(c, 0x70, bra(0x70, 0x70, 7, false));
    }
    return n;
}

// The shader blob: the 0x30-byte prefix our translator expects, the 0x50-byte program header (fragment, color target
// 0 written from R0..R3) and the code
static size_t blob(const Case *c, uint8_t out[0x80 + 128]) {
    memset(out, 0, 0x80 + 128);
    uint32_t h[20] = {0};
    h[0] = 2 | 3 << 5 | 5 << 10 | 3 << 17;
    h[18] = 0xf;
    memcpy(out + 0x30, h, sizeof h);
    for (int i = 0; i < 16; i++) {
        uint64_t w = c->used[i] ? c->code[i] : i % 4 == 0 ? 0 : NOP;
        memcpy(out + 0x80 + i * 8, &w, 8);
    }
    return 0x80 + 128;
}

static const char prelude[] =
    "#include <cstdint>\n#include <cstdio>\n#include <cstring>\n"
    "typedef uint32_t uint;\n"
    "template <class T, class U> static T as_type(U u) { static_assert(sizeof(T) == sizeof(U), \"size\"); T t; memcpy(&t, &u, sizeof t); return t; }\n"
    "static float as_f(uint u) { return as_type<float>(u); }\n"
    "struct u4 { uint v[4]; uint operator[](int i) const { return v[i]; } };\n"
    "static u4 cb3[1];\n"
    "static int fails;\n"
    "static void check(const char *name, float c, uint got, uint want) {\n"
    "    if (got != want) { printf(\"FAIL %s, c[3][0] = %g: R0 = 0x%08x, want 0x%08x\\n\", name, c, got, want); fails++; }\n"
    "}\n";

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s msl|glsl|structured <out.cpp>\n", argv[0]); return 2; }
    const char *mode = argv[1];
    bool glsl = strcmp(mode, "msl") != 0, structured = !strcmp(mode, "structured");
    setenv("LARECOMP_VK_STATIC_STACKS", structured ? "1" : "0", 1);
    setenv("LARECOMP_VK_STRUCTURE", structured ? "1" : "0", 1);
    FILE *o = fopen(argv[2], "w");
    if (!o) { perror(argv[2]); return 2; }
    fputs(prelude, o);
    Case cs[8];
    int n = cases(cs), bad = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b[0x80 + 128];
        size_t len = blob(&cs[i], b);
        ShaderKey key = {0};
        key.glsl = glsl;
        ShaderInfo *s = maxwell_translate(b, len, STAGE_FRAGMENT, &key);
        const char *start = !s->msl ? NULL : glsl ? strstr(s->msl, "void main() {\n") : strstr(s->msl, " main0(");
        start = start ? strstr(start, "{\n") : NULL;
        const char *end = start ? strstr(start, glsl ? "  c0[0] =" : "  FOut o;") : NULL;
        if (!end) { printf("FAIL %s (%s): translation: %s\n", cs[i].name, mode, s->msl ? "unexpected layout" : s->error); bad++; maxwell_free(s); continue; }
        // the mode really in use: the state machine switches on pc, the structured program has no pc
        bool has_pc = strstr(start, "switch (pc)") != NULL;
        if (has_pc == structured) {
            printf("FAIL %s (%s): %s\n", cs[i].name, mode, structured ? "kept the state machine" : "no state machine");
            bad++;
        }
        fprintf(o, "static uint case%d(float c) %.*s  return r0;\n}\n", i, (int)(end - start), start);
        maxwell_free(s);
    }
    fprintf(o, "int main() {\n    static const float cs[] = {1.0f, 0.0f, -0.5f, -8.0f};\n    for (float c : cs) {\n"
               "        cb3[0].v[0] = as_type<uint>(c);\n        uint want = c >= 0.0f ? 0x3f800000u : 0u;\n");
    for (int i = 0; i < n; i++) fprintf(o, "        check(\"%s (%s)\", c, case%d(c), want);\n", cs[i].name, mode, i);
    fprintf(o, "    }\n    printf(\"%s: %d cases x 4 values, %%d failures\\n\", fails);\n    return fails != 0;\n}\n", mode, n);
    fclose(o);
    return bad != 0;
}
