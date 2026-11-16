// Self-update for the Windows zip (x64 and ARM64) and the Linux AppImage (runtime/update.cpp, the core in
// update_core.h). The start screen's update dialog (runtime/ui/launcher.mm) runs it; macOS uses Sparkle instead
// (runtime/ui/sparkle.mm), Android and iOS have only the Download button.
//
// Update: the platform's signed manifest (update-<platform>.txt on the release page) is fetched and checked, the file
// downloaded and checked against it, the zip unpacked and every file checked, the app's files swapped (only the ones
// the manifest lists: on Windows the data folder is the exe's folder by default, and saves, seeds, settings and the
// game files there are never touched), and the app started again. Anything failing leaves the app as it was, and
// the dialog's Download button (the release page) always works.
//
// Test switches (docs/debug-hooks.md):
//   LARECOMP_UPDATE_URL   where releases are instead of GitHub's download URL: <url>/<tag>/<file> (http://127.0.0.1:...,
//                         file:///...)
//   LARECOMP_UPDATE_KEY   the public key (64 hex) manifests are checked with; only with LARECOMP_UPDATE_URL
//   LARECOMP_UPDATE_APP   Windows: the app folder to update (default: the exe's); Linux: $APPIMAGE stands for it
//   LARECOMP_UPDATE_NO_RESTART=1  quit after the swap instead of starting the new version
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The platforms that update themselves (others: Download only)
// (LARECOMP_SELF_UPDATE_TEST: tools/test_update_e2e.sh builds the AppImage path on any POSIX desktop)
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__)) || defined(LARECOMP_SELF_UPDATE_TEST)
#define LARECOMP_SELF_UPDATE 1
#else
#define LARECOMP_SELF_UPDATE 0
#endif

// main(), first thing after plat_init: finishes or undoes an update. A swap that was cut off, or a new version that
// started before and never reached the start screen, gets the old files back and the old version started (no return).
void update_boot(void);
// The start screen is up: an update that got this far is kept (the old files go).
void update_reached_start_screen(void);

// Whether Update can work here: 1 yes; 0 no (why: a short reason for the log - not this kind of build, the app's
// folder isn't writable, not run as an AppImage).
int update_available(const char **why);

// Starts the update in the background. tag: the release's tag, or "nightly" (then the manifest's version must be a
// nightly newer than min_nightly_stamp); platform: LARECOMP_PLATFORM.
void update_start(const char *tag, const char *platform, long long min_nightly_stamp);
void update_cancel(void);  // (while downloading)
enum { UPDATE_IDLE, UPDATE_RUNNING, UPDATE_FAILED, UPDATE_DONE };
// state; *frac the download's share done (-1: unknown); msg: what it's doing, or why it failed (English, short)
int update_status(double *frac, char *msg, int n);
// UPDATE_DONE: starts the new version and ends this process (returns only if the restart failed)
void update_restart(void);

#ifdef __cplusplus
}
#endif
