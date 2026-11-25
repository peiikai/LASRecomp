// Saves: export, import and the sync folder (runtime/saves.c; the start screen's Settings > Storage rows,
// runtime/ui/launcher.mm).
//
// A saves bundle is a .zip laid out as the data folder is, so it can also be unpacked by hand:
//   lasrecomp-saves.txt                         what made it (format, app and game version) and when each slot was saved
//   save/save_0/savedataN.bin (+ sidecars, screenshotN.bin)              the vanilla game's files 1-3 and autosave (N 0-3)
//   rando/<seed>/save/save_0/...                                          each seed's own slots
//   rando/<seed>/seed.json, exefs/*.pchtxt, spoiler.txt, ...              the seed itself, so it plays where it was never made
// Never game files: a seed folder's romfs/ (seeds made before seed.json) is left out, and so are those seeds.
//
// Import checks every save (size, version, both CRCs) before anything is written, zips the files it replaces into
// backups/ first, writes each file in one step (a temporary copy renamed over it) and takes a slot's sidecars with
// it, so a save and its .flagsx always match. It runs on the start screen only: the game holds no save open there.
//
// The sync folder: a folder the player picks (iCloud Drive, a Google Drive / Dropbox / OneDrive folder, any folder
// on Android through the system's picker) holding one bundle, "LAS Recomp saves.zip". After each save the game
// merges its slots into it on a background thread, slot by slot, the newer of the two kept (a slot's time is its
// file's, in UTC); on the start screen a slot newer there than here is offered to bring in, through the import above.
// One file rather than a folder of saves: a cloud drive that has synced half a folder would pair a save with another
// device's sidecar; a half-synced zip fails its own checks and is skipped.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SAVES_MAX_GAMES = 64, SAVES_SLOTS = 4 };

typedef struct {
    char seed[128];                 // "" the vanilla game, else the seed's folder name
    unsigned slots;                 // bit N: slot N (savedataN.bin) is in the bundle
    long long mtime[SAVES_SLOTS];   // when each was saved (seconds since 1970, UTC; 0 not known)
    bool seed_files;                // the bundle carries the seed itself
    const char *problem;            // NULL: can be imported; else why not (English, for L())
    bool seed_missing;              // (the problem: its seed is neither here nor in the bundle; a sync merge still
                                    //  carries it along for the devices that have the seed)
    unsigned newer;                 // saves_sync_check: the slots newer in the sync folder than here
} SavesGame;

typedef struct {
    int n;
    SavesGame g[SAVES_MAX_GAMES];
    char app[32], game[48];         // what made it (from its lasrecomp-saves.txt)
} SavesInfo;

// NULL when the save file's bytes are a save of this game, else why not ("damaged (body checksum)", ...)
const char *saves_check(const unsigned char *b, size_t n);

// Every save (vanilla and each seed's, with the seeds) into a new bundle at zip. app: the app's version, for its
// lasrecomp-saves.txt. false: err says why (nothing is left at zip).
bool saves_export(const char *zip, const char *app, char *err, size_t n);
// What the bundle at zip holds, each save checked. false: it isn't a saves bundle (err says why).
bool saves_inspect(const char *zip, SavesInfo *info, char *err, size_t n);
// The slots take[i] of game i of info (saves_inspect of the same zip) into the data folder: the files they replace
// zipped into backups/ first. false: err says why; a failure before the first write leaves everything as it was.
bool saves_import(const char *zip, const SavesInfo *info, const unsigned *take, char *err, size_t n);
// Any save at all in the data folder (Export has something to do)
bool saves_any(void);
// A slot as the start screen names it ("File 1" ... "Autosave"), English for L()
const char *saves_slot_name(int slot);
// The app's version, written into bundles (the start screen sets it)
void saves_app_version(const char *v);

// ---- the sync folder. Where it is lives in saves_sync.txt in the data folder: a path, on iOS the folder's
// security-scoped bookmark, on Android the folder's content:// URI (its permission kept by the activity).
void saves_sync_init(void);             // at start, in the data folder (main, before the first save). Off for test
                                        // runs (LARECOMP_SAVE_DIR)
bool saves_sync_on(void);
const char *saves_sync_shown(void);     // the folder as picked (a path; Android: the URI's own name for it); "" when off
// Use the folder (a path, or Android's URI) from now on and merge into it at once; NULL turns syncing off.
bool saves_sync_set(const char *where, char *err, size_t n);
// After the game committed a save (runtime/services/fs.c): a merge into the sync folder shortly, on its own thread
void saves_sync_kick(void);
// The start screen: fetches the sync folder's bundle and marks in info the slots newer there than here. true: any.
// Blocks (a cloud file may download first): call it on a thread of its own.
bool saves_sync_check(SavesInfo *info);
// Those newer slots in, through saves_import (backups first)
bool saves_sync_bring_in(const SavesInfo *info, char *err, size_t n);

// The platform's side of the sync folder: the folder's bundle copied to local, or local into the folder (as one step
// there). Windows / Linux: plain files (saves.c); macOS / iOS: coordinated with the cloud drive, so an iCloud file is
// downloaded first (platform/apple_sync.m); Android: through the storage framework (the activity, android_storage.c).
// fetch: 1 done, 0 no bundle there yet, -1 failed.
int plat_sync_fetch(const char *where, const char *name, const char *local);
bool plat_sync_put(const char *where, const char *name, const char *local);

#ifdef __cplusplus
}
#endif
