// The game code generator: turns one module of the game (exefs/main or exefs/sdk) into C files (runtime/gen/).
// Used in-process by the start screen's setup (runtime/gamebuild.c) and by the lasgen command-line tool (cli.cpp).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LasgenOptions {
    const char *name;    // "main" or "sdk": the file names and table symbols
    const char *nso;     // the module file
    uint64_t base;       // its load address
    const char *outdir;  // where the C goes (created)
    const char *hle;     // hle_symbols.txt (sdk), or NULL
    const char *hle_text;  // or its contents (then hle is not read)
    const char *patch_variants;  // the randomizer's code variants (tools/rando/patch_variants.json), or NULL
    const char *patch_variants_text;  // or its contents
    const char *simd;    // "asm" (AArch64 hosts) or "c"
    bool dyn_base;       // guest addresses relative to the window the runtime picks (Android, iOS)
    // cutting the files when the module has no saved cuts (or new_chunk_bounds)
    int chunk;           // functions per file at most (1500)
    int chunk_bytes;     // and about this much C (1500000)
    int big_fn;          // bytes of C from which a function gets its own file (100000)
    int limit;           // only the first `limit` functions (0: all; testing)
    bool new_chunk_bounds;           // cut by size and write the cuts to bounds_file
    const char *bounds_file;         // runtime/gen/chunk_bounds.inc (new_chunk_bounds only)
    // Each line the generator reports ("main: progress 120/45000 files 3" and the like), or NULL for stderr.
    void (*log)(void *ud, const char *line);
    void *ud;
    // Polled between functions: nonzero stops the run (it then fails with "cancelled").
    const volatile int *cancel;
} LasgenOptions;

// The defaults above (everything else zero).
void lasgen_defaults(LasgenOptions *o);

// Generates the module. false: why says what went wrong (one line, for the log and the setup's error).
bool lasgen_run(const LasgenOptions *o, char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
