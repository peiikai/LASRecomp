// The start screen's Game Files page on iOS (runtime/ui/ios_launcher.mm). The game code ships inside the app
// (Frameworks/LASGame.framework), so setting up is only bringing the player's exefs / romfs folders into
// <Documents>/extracted: they copy them into LAS Recomp's folder in the Files app (Finder / iTunes file sharing,
// AirDrop, a cloud drive), and Set up finds them anywhere in there, moves them into place (a rename: no copy) and
// checks them. Shares the launcher's busy state, job line and bar through the hooks below (as ios_export_ui.h).
#pragma once
#include <string>

struct IosLauncherHost {
    void (*lock)(void);                    // the launcher's state mutex (job, busy)
    void (*unlock)(void);
    bool *busy;                            // a setup is running (under lock)
    void (*set_job)(const std::string &line, bool failed, const std::string &hint);
    void (*set_failure)(int kind, int have);  // the launcher's failure texts (FAIL_*)
    void (*begin_background_work)(void);   // stops the background shader preparation, resets the bar
    void (*progress)(double fraction, const char *step);
    void (*end_background_work)(void);
    void (*game_files_changed)(void);      // the launcher's cached checks are stale
    // exefs: the folder with main and main.npdm, romfs: the one with region_common, in dir or a few levels below
    void (*find_game)(const std::string &dir, std::string &exefs, std::string &romfs, bool &unusable);
    int (*count_files)(const std::string &dir);
    bool (*check_version)(const std::string &exefs);
    int romfs_files;                       // how many files a complete romfs has
    const std::string *game_root;          // <Documents>/extracted
};
void ios_launcher_init(const IosLauncherHost &host);

// The page's rows before the game files are ready (option_rows.h, inside the launcher's opt_begin / opt_end)
void ios_game_files_rows(bool busy);
// The Files app, at LAS Recomp's folder
void ios_open_documents(void);
