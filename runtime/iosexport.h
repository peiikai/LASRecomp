// The iOS app exported from the start screen, on any desktop. The player unpacks the iOS skeleton zip of the release
// (tools/ios_skeleton.sh: the app without game code, the linkers, the compiler's Darwin headers) and picks that
// folder and their game files folder; the game code library is built here from those files (runtime/gamebuild.c
// GAMEBUILD_IOS), put in as Frameworks/LASGame.framework, and the whole thing zipped as an unsigned .ipa - the
// player signs it with their own tool and Apple ID (the one entitlement it needs is in the executable's ad-hoc
// signature, where such tools read it; a free team grants it). The game files go to the app's Documents on the device (Files / Finder).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*progress)(double fraction, const char *step);  // now and then, from the calling thread
    bool (*cancelled)(void);                              // optional
    int jobs;                                             // compiler processes at once
    const char *skeleton;                                 // the unpacked iOS skeleton folder
    const char *out_ipa;                                  // where to write it (an existing file is replaced)
} IosExportOpts;

// The export can run: the bundled compiler, and in the skeleton folder the app, headers and a linker for this
// machine. false: why.
bool iosexport_available(const char *skeleton, char *why, size_t why_size);
// Exports it (minutes: the game code build) from game_dir (exefs/ inside; the build's files go to <game_dir>/ios).
// false: why says what went wrong (details in the log).
bool iosexport_run(const char *game_dir, const IosExportOpts *o, char *why, size_t why_size);
// Where the start screen suggests saving it (~/Downloads/LAS Recomp-<version>-ios.ipa)
void iosexport_default_path(char *out, size_t n);
// `lasrecomp --build-ios <skeleton> [game files]` (runtime/main.c): the export without the start screen, progress on
// stderr, to iosexport_default_path or LARECOMP_IOS_OUT=<path>
bool iosexport_run_cli(const char *skeleton, const char *game_dir);

#ifdef __cplusplus
}
#endif
