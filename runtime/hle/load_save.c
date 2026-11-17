// --load-save <file 1-4 | savedataN.bin path> (debug): boot straight into a save, past the start screen, the title
// and the file select. LARECOMP_LOAD_SAVE=<same> does the same.
//
// The game loads the save itself: from the title we press what a player would (A skips the opening, L + R on the
// title), and on the file select the cursor goes to the file through the game's own move (the same hook as a tap on
// the touch screen, rt_file_nav_pick in runtime/ui/inventory_overlay.mm), then A, and A on "load this file?". So the
// game ends up exactly where it would after a normal load: the save's own spot, its flags, its seed's patches.
//   1-4    the file as numbered on the file select: 1-3 the diaries, 4 the autosave (savedata0-3.bin) in the save
//          folder of the game being started (the selected randomizer seed's, or the vanilla one)
//   path   a savedataN.bin from elsewhere (a reporter's): copied (with its .flagsx sidecar, if any) into a scratch save
//          folder, load_save/save_0/savedata0.bin in the data folder, and loaded as file 1. The game saves there too,
//          for the rest of the session: the player's own saves are never read or written. A path inside a seed's
//          folder (rando/<seed>/save/...) plays that seed; otherwise the selected seed, or LARECOMP_RANDO_SEED.
// The file is checked first (size, version, both CRCs): missing or bad -> one log line and the normal start, as if the
// option weren't given. Gives up (log line) if the game isn't in play 90 s after boot; never pressed again after that.
#include "../game_addresses.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../hle.h"
#include "../settings.h"

enum { SAVE_SIZE = 0x5EE0, SAVE_VERSION = 0xE };
#define SCRATCH "load_save"

static char g_arg[4096];  // the option as given (a path: made absolute where we started)
static int g_file = -1;   // the file to load, 0-3 (-1: off)
static int g_done;        // loaded, given up, or never on

static uint32_t la_crc(const uint8_t *p, size_t n) {  // (the game's CRC32: reflected, initial value 0xEDB88320, no
    uint32_t c = 0xEDB88320u;                         //  final XOR - notes/save_format.md)
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return c;
}

// Why the save at `path` can't be loaded, or NULL
static const char *save_problem(const char *path) {
    static uint8_t b[SAVE_SIZE];
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? "empty (no save in that file)" : strerror(errno);
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n < SAVE_SIZE) return "too short for a save";
    uint32_t v, hc, bc;
    memcpy(&v, b, 4), memcpy(&hc, b + 4, 4), memcpy(&bc, b + 0x5EC4, 4);
    if (v != SAVE_VERSION) return "not a save of this game (version)";
    memset(b + 4, 0, 4);
    if (la_crc(b, 0xE0) != hc) return "damaged (header checksum)";
    memset(b + 0x5EC4, 0, 4);
    if (la_crc(b + 0xE0, SAVE_SIZE - 0xE0) != bc) return "damaged (body checksum)";
    return NULL;
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    char buf[1 << 15];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) ok &= fwrite(buf, 1, n, out) == n;
    fclose(in);
    return fclose(out) == 0 && ok;
}

// The seed folder the boot will play (rando/<seed>), or NULL for vanilla: what runtime/rando.c's init picks. (Not
// rando_seed_dir(): that applies the seed's patches, and before the boot there is no game module to patch.)
static const char *seed_dir(void) {
    static char d[300];
    settings_load();
    const char *seed = getenv("LARECOMP_RANDO_SEED") ? getenv("LARECOMP_RANDO_SEED") : g_settings.rando_seed;
    if (!seed || !*seed) return NULL;
    snprintf(d, sizeof d, "rando/%s", seed);
    struct stat sb;
    return stat(d, &sb) || !S_ISDIR(sb.st_mode) ? NULL : d;
}

// (main, before the data root becomes the working directory) the option's value
void load_save_arg(const char *arg) {
    if (!realpath(arg, g_arg)) snprintf(g_arg, sizeof g_arg, "%s", arg);
}

static void off(const char *why) {
    LOG("[loadsave] %s: %s - normal start\n", g_arg, why);
    fprintf(stderr, "--load-save %s: %s - normal start\n", g_arg, why);
    g_file = -1, g_done = 1;
}

// (main, in the data root, before the first boot) check the save and set it up; false: off (a normal start)
bool load_save_prepare(void) {
    if (!g_arg[0]) {
        const char *e = getenv("LARECOMP_LOAD_SAVE");
        if (!e || !*e) return false;
        load_save_arg(e);
    }
    const char *a = g_arg;
    if (a[0] >= '1' && a[0] <= '4' && !a[1]) {  // a file of the game's own save folder
        g_file = a[0] - '1';
        const char *dir = getenv("LARECOMP_SAVE_DIR");
        char seed_save[600];
        if (!dir && seed_dir()) snprintf(seed_save, sizeof seed_save, "%s/save", seed_dir()), dir = seed_save;
        if (!dir) dir = "save";
        char path[1024];
        snprintf(path, sizeof path, "%s/save_0/savedata%d.bin", dir, g_file);
        const char *why = save_problem(path);
        if (why) {
            char m[1200];
            snprintf(m, sizeof m, "file %d (%s) %s", g_file + 1, path, why);
            off(m);
            return false;
        }
        LOG("[loadsave] loading file %d (%s)%s%s\n", g_file + 1, path, seed_dir() ? ", seed " : "", seed_dir() ? seed_dir() : "");
        return true;
    }
    struct stat sb;
    if (stat(a, &sb) || !S_ISREG(sb.st_mode)) { off(a[0] == '-' || !strchr(a, '/') ? "not a file 1-4 or a save file"
                                                                                    : "no such save file"); return false; }
    const char *why = save_problem(a);
    if (why) { off(why); return false; }
    // a seed's save (rando/<seed>/save/...): that seed
    const char *r = strstr(a, "/rando/");
    if (r && !getenv("LARECOMP_RANDO_SEED")) {
        char seed[256];
        if (sscanf(r + 7, "%255[^/]", seed) == 1 && strstr(r + 7 + strlen(seed), "/save/")) {
            setenv("LARECOMP_RANDO_SEED", seed, 1);
            LOG("[loadsave] the save is in seed %s's folder: playing that seed\n", seed);
        }
    }
    mkdir(SCRATCH, 0755);
    mkdir(SCRATCH "/save_0", 0755);
    for (int i = 0; i < 4; i++) {  // (only this save in the scratch folder: the other files show as empty)
        char p[256];
        snprintf(p, sizeof p, SCRATCH "/save_0/savedata%d.bin", i);
        remove(p);
        snprintf(p, sizeof p, SCRATCH "/save_0/savedata%d.bin.flagsx", i);
        remove(p);
        snprintf(p, sizeof p, SCRATCH "/save_0/screenshot%d.bin", i);
        remove(p);
    }
    if (!copy_file(a, SCRATCH "/save_0/savedata0.bin")) { off("couldn't copy it to the scratch save folder"); return false; }
    char side[4200];
    snprintf(side, sizeof side, "%s.flagsx", a);
    if (!stat(side, &sb) && copy_file(side, SCRATCH "/save_0/savedata0.bin.flagsx"))
        LOG("[loadsave] with its extra flags (%s)\n", side);
    setenv("LARECOMP_SAVE_DIR", SCRATCH, 1);  // (runtime/services/fs.c: the game's save folder for this session)
    g_file = 0;
    LOG("[loadsave] loading %s as file 1 (a copy in the scratch save folder %s; the game saves there this session)%s%s\n",
        a, SCRATCH, seed_dir() ? ", seed " : "", seed_dir() ? seed_dir() : "");
    return true;
}

// ---- driving the title and the file select (game thread)
extern int game_in_play(void);                 // runtime/hle/warp.c
extern int texpatch_title_screen(void);        // runtime/nvn/texpatch.c: 1 the title, 2 the file select, 0 else
extern bool movie_opening_playing(void);       // runtime/hle/movie.c
extern void file_select_jump(int file);        // runtime/ui/inventory_overlay.mm
extern uint64_t renderer_present_count(void);

static uint64_t g_cursor, g_fs_frame;  // the file select's cursor byte, and when the game last moved it (ran its list)

// (game thread, runtime/ui/inventory_overlay.mm) the file select ran its cursor this frame
void load_save_file_select(uint64_t cursor) {
    if (g_file < 0 || g_done) return;
    g_cursor = cursor, g_fs_frame = renderer_present_count();
}

// (game thread, each controller read) the buttons to add for the game: each press held 6 frames, then 24 let go
enum { A_BIT = 1 << 0, L_BIT = 1 << 6, R_BIT = 1 << 7, MAX_A = 4 };  // (runtime/hle/input.c's button bits)
uint64_t load_save_buttons(void) {
    if (g_file < 0 || g_done) return 0;
    uint64_t f = renderer_present_count();
    static uint64_t start, press_at, held;
    static int presses_a;
    if (!start) start = f + 1;
    if (game_in_play()) {
        uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
        LOG("[loadsave] file %d loaded: in play (%s), %llu frames after boot\n", g_file + 1,
            ps ? (const char *)(uintptr_t)ps : "?", (unsigned long long)(f + 1 - start));
        g_done = 1;
        return 0;
    }
    if (f + 1 - start > 90 * 60) {
        LOG("[loadsave] file %d: not in play 90 s after boot - stopped (the game is left as it is)\n", g_file + 1);
        g_done = 1;
        return 0;
    }
    if (press_at && f < press_at + 6) return held;
    if (press_at && f < press_at + 30) return 0;
    uint64_t want = 0;
    if (g_fs_frame && f - g_fs_frame < 10 && presses_a == 0) {  // the file select: the cursor to the file, then A
        if (LD8(g_cursor) != (uint8_t)g_file) { file_select_jump(g_file); return 0; }
        LOG("[loadsave] file select: file %d picked\n", g_file + 1);
        want = A_BIT;
    } else if (presses_a) {  // "load this file?" (its first choice loads it): A, a few times at most
        if (presses_a < MAX_A) want = A_BIT;
    } else if (movie_opening_playing()) {
        want = A_BIT;
    } else if (texpatch_title_screen() == 1) {
        want = L_BIT | R_BIT;
    }
    if (!want) return 0;
    if (want == A_BIT && (presses_a || (g_fs_frame && f - g_fs_frame < 10))) presses_a++;
    press_at = f, held = want;
    return want;
}
