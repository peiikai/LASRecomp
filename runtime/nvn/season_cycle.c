// The Cosmetics tab's Season: Random (issue #831): the seasons go round as you play, Spring -> Summer -> Autumn ->
// Winter -> Spring, LENGTH s each; a change fades the overworld's Leaves, Grass and Sand colours over FADE s
// (recolour_seasons.c blends the two seasons' tints). The clock runs as the weather cycle's does (weather_cycle.c):
// while the game is in play and draws its scene - indoors and in dungeons too, not on the file select or behind a
// menu. Weather "On" follows the season (weather.c).
// Saved with each save slot: savedataN.bin -> savedataN.bin.season, written with the save and matched to it by the
// save's body CRC like the weather cycle's file; a save without one (a new file, an older save) starts in Spring.
// Tests: LARECOMP_SEASON=random on without the setting; LARECOMP_SEASON_SPEED=<x> the clock x times faster;
// LARECOMP_SEASON_START=spring|summer|autumn|winter (a save without a file starts there, 20 s left)
#include "season.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../cosmetic_colour.h"
#include "../game_addresses.h"
#include "../hle.h"
#include "../settings.h"

extern int game_in_play(void);  // runtime/hle/warp.c

#define BODY_CRC 0x5EC4
#define LENGTH (15 * 60.0f)
#define FADE 20.0f
#define MAGIC 0x314E5353u  // "SSN1"

const int g_season_preset[N_SEASONS][3] = {
    {9, COL_DEFAULT, COL_DEFAULT}, {COL_DEFAULT, COL_DEFAULT, COL_DEFAULT}, {2, 3, 3}, {10, 10, 10}};

typedef struct {
    uint32_t magic, crc;  // (the save's body CRC this goes with)
    int32_t kind, from;   // the season and the one fading out (-1: none)
    float left, fade;     // s left in this season; s into the change (FADE: done)
} Cycle;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Cycle g_c;
static bool g_have;          // (g_c is set up)
static double g_scene_at;    // host time of the last scene composite (season_scene)
static uint32_t g_live_crc;  // body CRC of the save in play when we last matched it
static int g_was_in_play;
static struct { char path[1024]; Cycle c; } g_slots[8];  // (what each slot's file held, as read)
static int g_nslots;
// what the renderer shows (it asks per draw: no lock): bit 31 set, from (bits 0-3), to (4-7), the fade (8-23, /65535)
static _Atomic uint32_t g_show;

static double host_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static const char *name(int s) {
    return (const char *const[]){"spring", "summer", "autumn", "winter"}[s >= 0 && s < N_SEASONS ? s : 0];
}

static int env_mode(void) {  // LARECOMP_SEASON=random (test): 1
    static int env = -1;
    if (env < 0) env = getenv("LARECOMP_SEASON") && !strcmp(getenv("LARECOMP_SEASON"), "random");
    return env;
}
bool season_random_on(void) { return env_mode() || g_settings.season_random; }

static void publish(void) {  // (g_lock held)
    int from = g_have && g_c.from >= 0 && g_c.fade < FADE ? g_c.from : g_c.kind;
    float t = g_have && g_c.fade < FADE ? g_c.fade / FADE : 1;
    t = t * t * (3 - 2 * t);
    atomic_store(&g_show, 0x80000000u | (uint32_t)(from & 15) | (uint32_t)(g_c.kind & 15) << 4 |
                              (uint32_t)(t * 65535 + 0.5f) << 8);
}

static void fresh(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.magic = MAGIC, g_c.from = -1, g_c.fade = FADE;
    g_c.kind = SEASON_SPRING, g_c.left = LENGTH;
    const char *e = getenv("LARECOMP_SEASON_START");
    for (int i = 0; e && i < N_SEASONS; i++) if (!strcmp(e, name(i))) g_c.kind = i, g_c.left = 20;
    g_have = true;
}

RT_ON_RESET(season_cycle_reset) {
    pthread_mutex_lock(&g_lock);
    g_have = false, g_live_crc = 0, g_was_in_play = 0, g_nslots = 0;
    atomic_store(&g_show, 0);
    pthread_mutex_unlock(&g_lock);
}

void season_scene(void) { g_scene_at = host_now(); }

static uint32_t live_crc(void) { return *(const uint32_t *)(uintptr_t)(GA(LIVE_SAVE) + BODY_CRC); }

// a save came into play: its slot's season, or a fresh Spring
static void adopt(uint32_t crc) {
    g_live_crc = crc;
    for (int i = 0; i < g_nslots; i++)
        if (g_slots[i].c.crc == crc) {
            g_c = g_slots[i].c, g_c.from = -1, g_c.fade = FADE, g_have = true;
            LOG("[season] %s from %s (%.0f s left)\n", name(g_c.kind), g_slots[i].path, g_c.left);
            return;
        }
    fresh();
    LOG("[season] a save without one - %s (%.0f s)\n", name(g_c.kind), g_c.left);
}

void season_tick(void) {
    if (!season_random_on()) return;
    static double last;
    double now = host_now(), dt = now - last;
    last = now;
    static float speed = -1;
    if (speed < 0) speed = getenv("LARECOMP_SEASON_SPEED") ? (float)atof(getenv("LARECOMP_SEASON_SPEED")) : 1;
    pthread_mutex_lock(&g_lock);
    int play = game_in_play();
    uint32_t crc = play ? live_crc() : 0;
    if (play && (!g_have || crc != g_live_crc)) {  // another save in play (as weather_cycle_tick)
        bool slot = false;
        for (int i = 0; i < g_nslots; i++) slot |= g_slots[i].c.crc == crc;
        if (!g_have || !g_was_in_play || slot) adopt(crc);
        else g_live_crc = crc;
    }
    g_was_in_play = play;
    if (play && g_have && now - g_scene_at < 0.5 && dt > 0 && dt < 0.25) {
        float s = (float)dt * speed;
        if (g_c.fade < FADE) g_c.fade = g_c.fade + s < FADE ? g_c.fade + s : FADE;
        if ((g_c.left -= s) <= 0) {
            int n = (g_c.kind + 1) % N_SEASONS;
            LOG("[season] %s -> %s\n", name(g_c.kind), name(n));
            g_c.from = g_c.kind, g_c.kind = n, g_c.fade = 0, g_c.left = LENGTH;
        }
    }
    if (g_have) publish();
    pthread_mutex_unlock(&g_lock);
}

bool season_blend(int *from, int *to, float *t) {
    if (!season_random_on()) return false;
    uint32_t v = atomic_load(&g_show);
    if (!v) v = 0x80000000u | SEASON_SPRING | SEASON_SPRING << 4 | 65535u << 8;  // (no save in play yet)
    *from = (int)(v & 15), *to = (int)(v >> 4 & 15), *t = (v >> 8 & 65535) / 65535.0f;
    return true;
}

int season_current(void) {
    int from, to;
    float t;
    if (season_blend(&from, &to, &t)) return t < 0.5f ? from : to;
    for (int i = 0; i < N_SEASONS; i++)
        if (g_settings.leaves.pick == g_season_preset[i][0] && g_settings.grass.pick == g_season_preset[i][1] &&
            g_settings.sand.pick == g_season_preset[i][2])
            return i;
    return -1;
}

// ---- the save slots (runtime/services/fs.c)
static void slot_path(const char *path, char *out, size_t n) {  // (the save's own path; a working copy's ".pending" off)
    size_t len = strlen(path);
    bool pending = len > 8 && !strcmp(path + len - 8, ".pending");
    snprintf(out, n, "%.*s.season", (int)(len - (pending ? 8 : 0)), path);
}

// a save file was read into guest memory: its season, kept for when that save comes into play (read whatever the
// setting: Random may be turned on after the file select)
void season_save_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    if (off != 0 || size < BODY_CRC + 4) return;
    char p[1100];
    slot_path(path, p, sizeof p);
    Cycle c;
    FILE *f = fopen(p, "rb");
    bool ok = f && fread(&c, sizeof c, 1, f) == 1 && c.magic == MAGIC && c.kind >= 0 && c.kind < N_SEASONS &&
              c.crc == *(const uint32_t *)(uintptr_t)(addr + BODY_CRC);  // (else: from an older save of that slot)
    if (f) fclose(f);
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g_nslots && strcmp(g_slots[i].path, p)) i++;
    if (i == g_nslots && g_nslots < 8) snprintf(g_slots[g_nslots++].path, sizeof g_slots[0].path, "%s", p);
    if (i < g_nslots) {
        if (ok) g_slots[i].c = c;
        else memset(&g_slots[i].c, 0, sizeof g_slots[i].c);  // (crc 0 matches no save)
    }
    pthread_mutex_unlock(&g_lock);
}

// a save file was written from guest memory: the season now goes with it (replaced whole, as the weather cycle's)
void season_save_written(const char *path, uint64_t addr, int64_t off, int64_t size) {
    if (off != 0 || size < BODY_CRC + 4 || !season_random_on()) return;
    pthread_mutex_lock(&g_lock);
    if (!g_have) { pthread_mutex_unlock(&g_lock); return; }
    Cycle c = g_c;
    c.from = -1, c.fade = FADE;
    c.crc = *(const uint32_t *)(uintptr_t)(addr + BODY_CRC);
    g_live_crc = live_crc();  // (the game writes from a copy: the live save keeps its own CRC - not a new save)
    char p[1100], tmp[1200];
    slot_path(path, p, sizeof p);
    int i = 0;
    while (i < g_nslots && strcmp(g_slots[i].path, p)) i++;
    if (i == g_nslots && g_nslots < 8) snprintf(g_slots[g_nslots++].path, sizeof g_slots[0].path, "%s", p);
    if (i < g_nslots) g_slots[i].c = c;
    pthread_mutex_unlock(&g_lock);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(&c, sizeof c, 1, f) == 1;
    ok = !fclose(f) && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING);  // (rename() won't replace a file there)
#else
    ok = ok && !rename(tmp, p);
#endif
    if (!ok) { unlink(tmp); LOG("[season] could not write %s\n", p); }
}
