// Labyrinth (a dungeon room shuffle option: tools/rando/labyrinth.py; settings.json "dungeon-labyrinth": true). Every
// dungeon is a wing of one big dungeon (Color Dungeon = wing 0, dungeons 1-8 = wings 1-8). Here:
//
//   shared keys  ONE small-key counter for every wing. The game keeps a key count per dungeon record (live save +0xE0
//                + 0x10 * rec: dungeon n = record 2(n-1), Color Dungeon 18) and its locks / keyhole blocks spend from
//                the current level's. Every frame (game thread) a change to any wing's count - a key spent there, or
//                one the game added - is applied to all of them, so each wing holds the same count. Small keys found
//                (keysanity.c rt_add_item: index -1 everywhere in a Labyrinth seed, even on the overworld where the
//                game would drop them) go straight to every record. The number ever found is kept for the tracker
//                (logic counts keys found, not held) in extended flags 4084-4091 (runtime/hle/flags_ext.c, saved
//                per slot; the Chaos map letters keep 3800-4083 in a Labyrinth seed: doorjump.c).
//   last door    the overworld door Link last came in by (rando/<seed>/labyrinth_doors.tsv: door, the Field start
//                outside it, the level and spot it leads to): found by where Link stands once he's arrived from the
//                Field. Kept in flags 4092-4095 (door + 1). The Escape Bottle takes him back out of that door
//                (escape.c); none known yet: the wing's own entrance, as in every other seed.
//
// Doorways into another wing are door jumps that load a level (doorjump.c), the overworld doors and Eagle's Tower's
// way out are retargeted in the seed's room files (labyrinth.py write_doors).
#include "../game_addresses.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rando.h"

extern int game_in_play(void);
extern bool doorjump_player_xz(uint64_t frame, float *x, float *z);
bool flags_ext_get(int idx);
void flags_put(int idx, bool on);

#define RECORDS (GA_LIVE_SAVE + 0xE0)
static const int kRec[9] = {18, 0, 2, 4, 6, 8, 10, 12, 14};  // wing 0 (Color Dungeon), wings 1-8
enum { FOUND_FLAG = 4084, FOUND_BITS = 8, DOOR_FLAG = 4092, DOOR_BITS = 4 };

static int g_on = -1;
static bool g_synced;     // the records were made one count (once per save put in play)
static uint8_t g_last[9]; // each wing record's count as of the last sync

typedef struct { char region[24], field[24], level[24], start[32]; float x, z; } Door;
static Door g_doors[10];
static int g_ndoors = -1;

RT_ON_RESET(labyrinth_reset) { g_on = -1; g_synced = false; g_ndoors = -1; }

bool labyrinth_on(void) {
    if (g_on >= 0) return g_on;
    g_on = 0;
    const char *dir = rando_seed_dir();
    if (!dir) return false;
    char path[512], buf[16384];
    snprintf(path, sizeof path, "%s/settings.json", dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    g_on = strstr(buf, "\"dungeon-labyrinth\": true") != NULL;
    if (g_on) LOG("[labyrinth] on: one shared small-key counter\n");
    return g_on;
}

static int bits_get(int base, int n) {
    int v = 0;
    for (int b = 0; b < n; b++) v |= flags_ext_get(base + b) << b;
    return v;
}
static void bits_put(int base, int n, int v) {
    for (int b = 0; b < n; b++)
        if (flags_ext_get(base + b) != ((v >> b) & 1)) flags_put(base + b, (v >> b) & 1);
}

// small keys found so far, and held now (every wing holds the same count)
void labyrinth_keys(int *held, int *found) {
    *held = LD8(RECORDS + 0x10 * kRec[1]);
    *found = bits_get(FOUND_FLAG, FOUND_BITS);
}

static void put_all(int v) {
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    for (int i = 0; i < 9; i++) {
        *(volatile uint8_t *)(uintptr_t)(RECORDS + 0x10 * kRec[i]) = (uint8_t)v;
        g_last[i] = (uint8_t)v;
    }
}

static void add_found(int n) {
    int f = bits_get(FOUND_FLAG, FOUND_BITS) + n;
    bits_put(FOUND_FLAG, FOUND_BITS, f > 255 ? 255 : f);
}

// keysanity.c rt_add_item: a small key (any index) - 1 when taken here
int labyrinth_add_key(int count) {
    if (!labyrinth_on()) return 0;
    int n = count > 0 ? count : 1;
    if (!g_synced) {  // (before the first sync of this save: make the records one count first)
        int most = 0;
        for (int i = 0; i < 9; i++) most = LD8(RECORDS + 0x10 * kRec[i]) > most ? LD8(RECORDS + 0x10 * kRec[i]) : most;
        put_all(most);
        g_synced = true;
    }
    put_all(g_last[0] + n);
    add_found(n);
    LOG("[labyrinth] small key x%d: %d held, %d found\n", n, g_last[0], bits_get(FOUND_FLAG, FOUND_BITS));
    return 1;
}

static void load_doors(void) {
    g_ndoors = 0;
    const char *dir = rando_seed_dir();
    if (!dir) return;
    char path[512], line[256];
    snprintf(path, sizeof path, "%s/labyrinth_doors.tsv", dir);
    FILE *f = fopen(path, "r");
    while (f && g_ndoors < 10 && fgets(line, sizeof line, f)) {
        Door *d = &g_doors[g_ndoors];
        if (line[0] != '#' && sscanf(line, "%23s %23s %23s %31s %f %f", d->region, d->field, d->level, d->start, &d->x,
                                     &d->z) == 6)
            g_ndoors++;
    }
    if (f) fclose(f);
}

// The Escape Bottle's way out of the Labyrinth (escape.c): the Field start outside the door Link last came in by;
// false when none is known (the wing's own entrance then)
bool labyrinth_escape_target(char *level, size_t ln, char *locator, size_t lc) {
    if (!labyrinth_on()) return false;
    if (g_ndoors < 0) load_doors();
    int d = bits_get(DOOR_FLAG, DOOR_BITS) - 1;
    if (d < 0 || d >= g_ndoors) return false;
    snprintf(level, ln, "Field");
    snprintf(locator, lc, "%s", g_doors[d].field);
    return true;
}

static bool wing_level(const char *lv) {
    return !strncmp(lv, "Lv", 2) && strncmp(lv, "Lv09", 4) && lv[2] >= '0' && lv[2] <= '1';
}

// Game thread, every frame (warp.c give_item_tick)
void labyrinth_game_tick(uint64_t frame) {
    if (!labyrinth_on()) return;
    if (!game_in_play()) { g_synced = false; return; }
    // ---- the shared key counter
    uint8_t cur[9];
    for (int i = 0; i < 9; i++) cur[i] = LD8(RECORDS + 0x10 * kRec[i]);
    if (!g_synced) {  // a save just put in play: its records are one count already (else the highest)
        int most = 0;
        for (int i = 0; i < 9; i++) most = cur[i] > most ? cur[i] : most;
        put_all(most);
        g_synced = true;
        LOG("[labyrinth] save in play: %d small keys held, %d found\n", most, bits_get(FOUND_FLAG, FOUND_BITS));
    } else {
        int delta = 0, gained = 0;
        for (int i = 0; i < 9; i++) {
            int d = (int)cur[i] - (int)g_last[i];
            delta += d;
            if (d > 0) gained += d;
        }
        if (delta || gained) {
            put_all(g_last[0] + delta);
            if (gained) add_found(gained);  // (a key the game added itself)
            LOG("[labyrinth] small keys %+d: %d held, %d found\n", delta, g_last[0], bits_get(FOUND_FLAG, FOUND_BITS));
        }
    }
    // ---- the door Link came in by: arrived in a wing from the Field, he stands at one door's spot
    static char prev[40];
    static int pending;  // frames left to find where he arrived
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
    const char *lv = ps ? (const char *)(uintptr_t)ps : "";
    if (strcmp(prev, lv)) {
        if (!strcmp(prev, "Field") && wing_level(lv)) pending = 600;
        else pending = 0;
        strlcpy(prev, lv, sizeof prev);
    }
    if (pending > 0) {
        pending--;
        float x, z;
        if (g_ndoors < 0) load_doors();
        if (doorjump_player_xz(frame, &x, &z)) {
            int best = -1;
            float bd = 6.0f;  // (he may have walked in a few steps)
            for (int i = 0; i < g_ndoors; i++) {
                float d = hypotf(g_doors[i].x - x, g_doors[i].z - z);
                if (!strcmp(g_doors[i].level, lv) && d < bd) bd = d, best = i;
            }
            if (best >= 0) {
                bits_put(DOOR_FLAG, DOOR_BITS, best + 1);
                LOG("[labyrinth] came in by the %s door (%s / %s)\n", g_doors[best].region, lv, g_doors[best].start);
            } else
                LOG("[labyrinth] arrived in %s at (%.1f, %.1f): no door's spot\n", lv, x, z);
            pending = 0;
        }
    }
}
