// Host driver for tools/test_spirv_varyings.py: runs spirv_compact_varyings on a vertex + fragment SPIR-V pair.
// Usage: test_spirv_varyings <vs.spv> <fs.spv> <limit> <vs out> <fs out>; prints "<result> <info>".
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/nvn/vulkan/spirv_varyings.h"

static uint32_t *load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)sz);
    if (fread(w, 1, (size_t)sz, f) != (size_t)sz) { perror(path); exit(2); }
    fclose(f);
    *n = (size_t)sz / 4;
    return w;
}

static void store(const char *path, const uint32_t *w, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(w, 4, n, f) != n) { perror(path); exit(2); }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 6) { fprintf(stderr, "usage: %s vs.spv fs.spv limit vs-out fs-out\n", argv[0]); return 2; }
    size_t nv, nf;
    uint32_t *vs = load(argv[1], &nv), *fs = load(argv[2], &nf);
    char info[256];
    int r = spirv_compact_varyings(vs, nv, fs, nf, (uint32_t)atoi(argv[3]), info, sizeof info);
    store(argv[4], vs, nv);
    store(argv[5], fs, nf);
    printf("%d %s\n", r, info);
    return 0;
}
