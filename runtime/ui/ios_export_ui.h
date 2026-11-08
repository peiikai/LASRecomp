// The start screen's "Recomp for iOS" page (runtime/ui/ios_export_ui.mm): the iOS app exported with the game code
// built here (runtime/iosexport.h) from what the player picks - the iOS kit (.laskit) of the release and a game files
// folder - saved as an unsigned .ipa where they choose. Reached from Settings > Prelaunch; shares the launcher's
// busy state, job line and progress bar (the launcher's hooks below).
#pragma once
#include <string>

struct SDL_Window;

// The launcher's pieces the export uses (runtime/ui/launcher.mm)
struct IosExportHost {
    void (*lock)(void);                    // the launcher's state mutex (job, busy)
    void (*unlock)(void);
    bool *busy;                            // a setup / export is running (under lock)
    void (*set_job)(const std::string &line, bool failed, const std::string &hint);
    void (*begin_background_work)(void);   // stops the background shader preparation, resets the progress bar
    void (*progress)(double fraction, const char *step);  // the build's progress for the bar
    bool (*cancelled)(void);               // the overlay's Cancel was pressed
    void (*end_background_work)(void);     // the bar's end
    void (*reveal)(const std::string &dir);  // Finder / Explorer / the file manager
    const std::string *game_root;          // the set-up game files (the default game files folder when ready)
};
void ios_export_ui_init(const IosExportHost &host);

// The page's rows (option_rows.h, inside the launcher's opt_begin / opt_end). ready: the set-up game files can be
// played; busy: the launcher's busy state.
void ios_export_rows(bool ready, bool busy, SDL_Window *win);
// Settings > Prelaunch: the "Recomp for iOS..." button and, after an export, where it went
void ios_export_prelaunch(bool busy);
// The Prelaunch button was pressed since the last call (the launcher opens the page)
bool ios_export_page_requested(void);
