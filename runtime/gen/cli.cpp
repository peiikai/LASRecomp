// lasgen: the game code generator on the command line (the same code the start screen's setup runs in-process).
//
// Usage: lasgen <name> <nso> <base> <outdir> [--hle hle_symbols.txt] [--patch-variants patch_variants.json]
//               [--simd asm|c] [--dyn-base] [--new-chunk-bounds [--bounds-file F]] [--chunk N] [--chunk-bytes N]
//               [--big-fn N] [--limit N]
// The arguments are those of tools/gen.py; --patch-variants defaults to tools/rando/patch_variants.json as gen.py
// reads it (relative to the current folder here).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lasgen.h"

static int usage() {
    fprintf(stderr,
            "usage: lasgen <name> <nso> <base> <outdir> [--hle FILE] [--patch-variants FILE] [--simd asm|c] [--dyn-base]\n"
            "              [--new-chunk-bounds [--bounds-file FILE]] [--chunk N] [--chunk-bytes N] [--big-fn N] [--limit N]\n");
    return 2;
}

int main(int argc, char **argv) {
    LasgenOptions o;
    lasgen_defaults(&o);
#if defined(__aarch64__) || defined(_M_ARM64)
    o.simd = "asm";
#else
    o.simd = "c";
#endif
    o.patch_variants = "tools/rando/patch_variants.json";
    const char *pos[4];
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "lasgen: %s needs a value\n", a);
                exit(2);
            }
            return argv[++i];
        };
        if (!strcmp(a, "--hle")) o.hle = val();
        else if (!strcmp(a, "--patch-variants")) o.patch_variants = val();
        else if (!strcmp(a, "--simd")) {
            o.simd = val();
            if (strcmp(o.simd, "asm") && strcmp(o.simd, "c")) return usage();
        } else if (!strcmp(a, "--dyn-base")) o.dyn_base = true;
        else if (!strcmp(a, "--new-chunk-bounds")) o.new_chunk_bounds = true;
        else if (!strcmp(a, "--bounds-file")) o.bounds_file = val();
        else if (!strcmp(a, "--chunk")) o.chunk = atoi(val());
        else if (!strcmp(a, "--chunk-bytes")) o.chunk_bytes = atoi(val());
        else if (!strcmp(a, "--big-fn")) o.big_fn = atoi(val());
        else if (!strcmp(a, "--limit")) o.limit = atoi(val());
        else if (a[0] == '-' && a[1]) return usage();
        else if (npos < 4) pos[npos++] = a;
        else return usage();
    }
    if (npos != 4) return usage();
    o.name = pos[0];
    o.nso = pos[1];
    o.base = strtoull(pos[2], nullptr, 0);
    o.outdir = pos[3];
    char why[1024];
    if (!lasgen_run(&o, why, sizeof why)) {
        fprintf(stderr, "%s\n", why);
        return 1;
    }
    return 0;
}
