// The recompiled game code: a library built from the player's own game files (`make gen` + `make` in a source
// checkout, the start screen's setup otherwise; runtime/gamebuild.c) and loaded at startup (runtime/gamelib.c).
// Nothing from the game ships with the app.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#if defined(_WIN32)
#define GAMELIB_NAME "lasgame.dll"
#define GAMELIB_EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
#define GAMELIB_NAME "lasgame.dylib"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#else
#define GAMELIB_NAME "lasgame.so"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#endif

// GAMELIB_ID (-D, from the Makefile): a hash of the generator and the headers the game code is compiled against. A
// library built with another one is rebuilt, not loaded.
#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif
#define GAMELIB_ABI 1

typedef struct {
    uint32_t abi;    // GAMELIB_ABI
    const char *id;  // GAMELIB_ID
    const uint64_t *main_addrs, *sdk_addrs;  // sorted guest addresses
    const GuestFn *main_fns, *sdk_fns;       // the functions at them
    const uint64_t *main_count, *sdk_count;
} GameLibTables;

// The library's one exported function (tools/gamelib_entry.c).
typedef const GameLibTables *(*GameLibEntry)(void);
#define GAMELIB_ENTRY "lasgame_tables"

#ifdef __cplusplus
extern "C" {
#endif

// Loads the game code for the game files in game_dir (once; later calls return the first result). Looks at
// LARECOMP_GAME_LIB, next to the executable (a source checkout's build/), then game_dir. false: why says what's wrong.
bool gamelib_load(const char *game_dir, char *why, size_t why_size);
// The tables of the loaded library (NULL before gamelib_load succeeds).
const GameLibTables *gamelib_tables(void);
// True when a library usable with this build is in one of those places (without loading it).
bool gamelib_present(const char *game_dir);

// The CPU the game code is compiled for on this machine (x86-64; elsewhere "" and no flags). The start screen's build
// detects the best instruction set this CPU has (x86-64-v3 = AVX2 + FMA where there is one; otherwise SSE4.2 / SSE2
// plus AVX, F16C, FMA when present, e.g. "x86-64-v2+avx+f16c" on Ivy Bridge) and the id says which, so moving the
// game folder to another CPU rebuilds it instead of crashing. march: clang flags; zig: zig cc's -mcpu flag.
typedef struct {
    char tag[64];     // "" or e.g. "x86-64-v2+avx+f16c"
    char march[128];  // "" or e.g. "-march=x86-64-v2 -mavx -mf16c"
    char zig[128];    // "" or e.g. "-mcpu=x86_64_v2+avx+f16c"
} GameLibCpu;
const GameLibCpu *gamelib_cpu(void);
// GAMELIB_ID plus the CPU tag: the id of a library built here by the start screen.
const char *gamelib_id(void);

#ifdef __cplusplus
}
#endif
