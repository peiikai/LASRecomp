// The start screen's saves rows (runtime/ui/saves_ui.mm): Export saves, Import saves, Sync folder and the data
// folder's Show in Finder / Explorer / Files, under Settings > Storage, and their windows (the import's list, the
// sync folder's newer saves). The work itself is runtime/saves.c.
#pragma once
#include <string>

struct SDL_Window;
struct ImFont;

// The launcher's pieces the rows use (runtime/ui/launcher.mm)
struct SavesUiHost {
    ImFont *const *font_small, *const *font_menu;  // the popups' fonts (set once the fonts are loaded)
    SDL_Window *const *win;                        // the window the pickers open over
    void (*reveal)(const std::string &dir);        // Finder / Explorer / the Files app
    std::string (*shown)(const char *path);        // a folder as the player knows it (iOS: as Files names it)
    const char *app_version;
};
// Once, at the start screen: also checks the sync folder for newer saves (on a thread of its own)
void saves_ui_init(const SavesUiHost &host);
// The rows (inside the launcher's opt_begin / opt_end); busy: the launcher's setup is running
void saves_rows(bool busy);
// The windows, in front of everything (the start screen's top level, with the menus' look); s: the window's scale
void saves_popups(float s);
