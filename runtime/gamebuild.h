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
// exported app, runtime/iosexport.c - the bundled zig compiles for arm64 iOS, the skeleton's lld links)
enum { GAMEBUILD_HOST = 0, GAMEBUILD_IOS = 1 };

typedef struct {
    // now and then, from the calling thread: how far the whole build is (0..1) and what it's doing
    void (*progress)(double fraction, const char *step);
    int jobs;  // compiler processes at once (>= 1)
    bool (*cancelled)(void);  // optional, polled on this thread; completed objects stay reusable
    int target;  // GAMEBUILD_*
    const char *ios_skeleton;  // GAMEBUILD_IOS: the unpacked iOS skeleton folder (its headers, linker, link stub)
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log). game_dir holds exefs/ (and gets
// .gamebuild/ and the library): the iOS build uses its own folder (runtime/iosexport.c) so the two don't mix.
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);
// After a failed gamebuild_run: a helper was killed by a signal (e.g. out of memory); why then says so, for the player.
bool gamebuild_killed(void);
// The generator and the compiler are there (shipped builds; a source checkout with tools/venv and clang).
bool gamebuild_available(void);
// The iOS build's tools: the bundled zig, and in the skeleton folder its Darwin headers, a linker for this machine
// and the libSystem stub. false: why says what's missing, for the player.
bool gamebuild_ios_available(const char *skeleton, char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
