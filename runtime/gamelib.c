// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if defined(__x86_64__)
#include <cpuid.h>
#endif

#include "paths.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

#if defined(__x86_64__)
// CPUID (the features and whether the OS saves the AVX registers), read directly: no compiler runtime support needed.
typedef struct { bool sse3, ssse3, sse41, sse42, popcnt, cx16, lahf, avx, f16c, fma, avx2, bmi1, bmi2, lzcnt, movbe; } X86;
static X86 x86_features(void) {
    X86 f = {0};
    unsigned a, b, c, d;
    __cpuid(0, a, b, c, d);
    unsigned max = a;
    if (max < 1) return f;
    __cpuid(1, a, b, c, d);
    f.sse3 = c >> 0 & 1, f.ssse3 = c >> 9 & 1, f.cx16 = c >> 13 & 1, f.sse41 = c >> 19 & 1, f.sse42 = c >> 20 & 1;
    f.movbe = c >> 22 & 1, f.popcnt = c >> 23 & 1;
    bool fma = c >> 12 & 1, f16c = c >> 29 & 1, avx = c >> 28 & 1, osxsave = c >> 27 & 1;
    if (avx && osxsave) {
        unsigned lo, hi;
        __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0));  // xgetbv(0)
        f.avx = (lo & 6) == 6;  // the OS saves XMM and YMM
    }
    f.fma = f.avx && fma, f.f16c = f.avx && f16c;
    if (max >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        f.avx2 = f.avx && (b >> 5 & 1), f.bmi1 = b >> 3 & 1, f.bmi2 = b >> 8 & 1;
    }
    __cpuid(0x80000000, a, b, c, d);
    if (a >= 0x80000001) {
        __cpuid(0x80000001, a, b, c, d);
        f.lahf = c & 1, f.lzcnt = c >> 5 & 1;
    }
    return f;
}
#endif

const GameLibCpu *gamelib_cpu(void) {
    static GameLibCpu c;
    static bool done;
    if (done) return &c;
    done = true;
#if defined(__x86_64__)
    X86 f = x86_features();
    bool v2 = f.sse3 && f.ssse3 && f.sse41 && f.sse42 && f.popcnt && f.cx16 && f.lahf;
    bool v3 = v2 && f.avx && f.avx2 && f.bmi1 && f.bmi2 && f.f16c && f.fma && f.lzcnt && f.movbe;
    int level = v3 ? 3 : v2 ? 2 : 1;
    snprintf(c.tag, sizeof c.tag, "%s", level == 3 ? "x86-64-v3" : level == 2 ? "x86-64-v2" : "x86-64");
    snprintf(c.march, sizeof c.march, "-march=%s", c.tag);
    snprintf(c.zig, sizeof c.zig, "-mcpu=%s", level == 3 ? "x86_64_v3" : level == 2 ? "x86_64_v2" : "x86_64");
    // what the level lacks but this CPU has (Sandy / Ivy Bridge: AVX, F16C; Piledriver: + FMA; Penryn: SSE4.1)
    const struct { bool has; const char *name, *flag, *zig; int upto; } extra[] = {
        {f.sse3, "sse3", "-msse3", "sse3", 1},    {f.ssse3, "ssse3", "-mssse3", "ssse3", 1},
        {f.sse41, "sse4.1", "-msse4.1", "sse4_1", 1}, {f.avx, "avx", "-mavx", "avx", 2},
        {f.f16c, "f16c", "-mf16c", "f16c", 2},    {f.fma, "fma", "-mfma", "fma", 2},
    };
    for (size_t i = 0; i < sizeof extra / sizeof *extra; i++) {
        if (!extra[i].has || level > extra[i].upto) continue;
        size_t n = strlen(c.tag), m = strlen(c.march), z = strlen(c.zig);
        snprintf(c.tag + n, sizeof c.tag - n, "+%s", extra[i].name);
        snprintf(c.march + m, sizeof c.march - m, " %s", extra[i].flag);
        snprintf(c.zig + z, sizeof c.zig - z, "+%s", extra[i].zig);
    }
#endif
    return &c;
}

const char *gamelib_id(void) {
    static char id[160];
    if (!id[0]) {
        const GameLibCpu *c = gamelib_cpu();
        snprintf(id, sizeof id, "%s%s%s", GAMELIB_ID, c->tag[0] ? "-" : "", c->tag);
    }
    return id;
}

// Built for this machine (gamelib_id), or by `make` (its flags, GENARCH, are the developer's: "-make").
static bool id_ok(const char *id) { return !strcmp(id, gamelib_id()) || !strcmp(id, GAMELIB_ID "-make"); }

// The library's id file (<library>.id, written next to it by the build): its GAMELIB_ID, read without loading it.
static bool id_matches(const char *lib) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    return id_ok(id);
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
    const char *env = getenv("LARECOMP_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);
    char exe_side[1024];
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
    return n;
}

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[3][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i])) return true;
    return false;
}

static const GameLibTables *open_lib(const char *path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || !id_ok(t->id)) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char c[3][1100];
        int n = candidates(game_dir, c);
        const char *env = getenv("LARECOMP_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            if (!exists(c[i])) continue;
            bool given = i == 0 && env && *env;  // LARECOMP_GAME_LIB: loaded as given, the library's own id decides
            if (!given && !id_matches(c[i])) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            g_tables = open_lib(c[i]);
            if (g_tables) LOG("[gamelib] %s (%s, %llu + %llu functions)\n", c[i], g_tables->id, (unsigned long long)*g_tables->main_count,
                              (unsigned long long)*g_tables->sdk_count);
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
