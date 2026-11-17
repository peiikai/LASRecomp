// iOS: SDL's file dialogs and SDL_OpenURL as UIKit's (runtime/platform/ios_picker.m). SDL 3 has no dialog backend
// on iOS (its dialogs call back with NULL there), so the start screen's calls are renamed to these by macro and the
// code stays the desktop's. Include it anywhere, before or after SDL.h: it brings SDL in itself.
#pragma once
#include <SDL3/SDL.h>
#include "../platform.h"

#if PLAT_IOS
#ifdef __cplusplus
extern "C" {
#endif
// UIDocumentPickerViewController for a folder: Files (On My iPhone, iCloud Drive, other apps' folders, external
// drives). The picked folder is kept under security-scoped access until the next pick or exit, so it can be read
// and a bookmark of it made. The callback is on the main thread: one path, an empty list for a cancel, NULL when
// no window could be shown - as SDL's; default_location is ignored.
void ios_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                              const char *default_location, bool allow_many);
// Files with the filters' extensions ("bfttf;ttf", as SDL's patterns; "*" any), picked as copies in the app's tmp
// folder (no security scope needed). The callback as above, with every path picked.
void ios_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                            const SDL_DialogFileFilter *filters, int nfilters, const char *default_location, bool allow_many);
// A file:// URL to a folder opens the Files app there (shareddocuments://); anything else as SDL opens it.
bool ios_OpenURL(const char *url);
#ifdef __cplusplus
}
#endif
#define SDL_ShowOpenFolderDialog ios_ShowOpenFolderDialog
#define SDL_ShowOpenFileDialog ios_ShowOpenFileDialog
#define SDL_OpenURL ios_OpenURL
#endif
