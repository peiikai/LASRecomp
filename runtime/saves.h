// Saves: export and import (runtime/saves.c; the start screen's Settings > Storage rows,
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

#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SAVES_SLOTS = 4 };

typedef struct {
    char seed[128];                 // "" the vanilla game, else the seed's folder name
    unsigned slots;                 // bit N: slot N (savedataN.bin) is in the bundle
    long long mtime[SAVES_SLOTS];   // when each was saved (seconds since 1970, UTC; 0 not known)
    bool seed_files;                // the bundle carries the seed itself
    const char *problem;            // NULL: can be imported; else why not (English, for L())
} SavesGame;

typedef struct {
    int n;
    SavesGame *g;                   // n games (saves_info_free)
    char app[32], game[48];         // what made it (from its lasrecomp-saves.txt)
} SavesInfo;

// NULL when the save file's bytes are a save of this game, else why not ("damaged (body checksum)", ...)
const char *saves_check(const unsigned char *b, size_t n);

// Every save (vanilla and each seed's, with the seeds) into a new bundle at zip. app: the app's version, for its
// lasrecomp-saves.txt. false: err says why (nothing is left at zip).
bool saves_export(const char *zip, const char *app, char *err, size_t n);
// What the bundle at zip holds, each save checked. false: it isn't a saves bundle (err says why). info starts zeroed
// or holds an earlier saves_inspect, which is freed first.
bool saves_inspect(const char *zip, SavesInfo *info, char *err, size_t n);
void saves_info_free(SavesInfo *info);
// The slots take[i] (info->n of them) of game i of info (saves_inspect of the same zip) into the data folder: the files they replace
// zipped into backups/ first. false: err says why; a failure before the first write leaves everything as it was.
bool saves_import(const char *zip, const SavesInfo *info, const unsigned *take, char *err, size_t n);
// Any save at all in the data folder (Export has something to do)
bool saves_any(void);
// A slot as the start screen names it ("File 1" ... "Autosave"), English for L()
const char *saves_slot_name(int slot);
// The app's version, written into bundles (the start screen sets it)
void saves_app_version(const char *v);

#ifdef __cplusplus
}
#endif
