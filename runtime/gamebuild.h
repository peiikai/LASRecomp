// Builds the game code library (runtime/gamelib.h) on the player's machine, from their own game files: the generator
// (tools/gen.py; shipped frozen as lasgen/) turns exefs/main and exefs/sdk into C, the bundled compiler (zig/) builds
// that into <game_dir>/lasgame.*. The compiled pieces are kept in <game_dir>/.gamebuild: an update compiles only the
// pieces whose generated C changed.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the library is built for: this machine (the game plays here), or iOS (any desktop: the library for the
// exported app, runtime/iosexport.c - the bundled zig compiles for arm64 iOS, the kit's lld links)
enum { GAMEBUILD_HOST = 0, GAMEBUILD_IOS = 1 };

typedef struct {
    // now and then, from the calling thread: how far the whole build is (0..1) and what it's doing
    void (*progress)(double fraction, const char *step);
    int jobs;  // compiler processes at once (>= 1)
    bool (*cancelled)(void);  // optional, polled on this thread; completed objects stay reusable
    int target;  // GAMEBUILD_*
    const char *ios_kit;  // GAMEBUILD_IOS: the iOS kit (.laskit, tools/ios_kit.sh: its headers, linker, link stub)
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log). game_dir holds exefs/ (and gets
// .gamebuild/ and the library): the iOS build uses its own folder in the data root's cache (runtime/iosexport.c) so
// the two don't mix.
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);
// After a failed gamebuild_run: a helper was killed by a signal (e.g. out of memory); why then says so, for the player.
bool gamebuild_killed(void);
// After a failed gamebuild_run: Windows security settings (Control Flow Guard) stopped the generator (#337).
bool gamebuild_cfg_stopped(void);
// The generator and the compiler are there (shipped builds; a source checkout with tools/venv and clang).
bool gamebuild_available(void);
// The iOS build's tools: the bundled zig, and in the kit (a .laskit file, never a folder) the app, its Darwin
// headers, a linker for this machine and the libSystem stub. false: why says what's missing, for the player. The
// build unpacks this machine's tools once per kit into <data root>/cache/ios-kit/<kit id>/.
bool gamebuild_ios_available(const char *kit, char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
