// The Cosmetics tab's Season row (issue #831): a fixed season fills the Leaves, Grass and Sand rows; Random goes round
// the seasons as you play (runtime/nvn/season_cycle.c), fading the overworld's colours from one to the next
// (recolour_seasons.c). Weather "On" picks the weather from the season (weather.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { SEASON_SPRING, SEASON_SUMMER, SEASON_AUTUMN, SEASON_WINTER, N_SEASONS };

// The seasons' Leaves, Grass and Sand picks (palette: COL_DEFAULT, Orange 2, Gold 3, Pink 9, White 10): Spring cherry
// blossom, Summer the game's own, Autumn orange / gold / gold, Winter white
extern const int g_season_preset[N_SEASONS][3];

bool season_random_on(void);  // Season: Random (or LARECOMP_SEASON=random)
// The season showing now: Random's, else the one the Leaves / Grass / Sand rows match (-1: none of them)
int season_current(void);
// Random: the season fading out, the one coming in and how far (0..1, eased; from == to: no change on). False: off.
bool season_blend(int *from, int *to, float *t);

void season_tick(void);   // game thread, every frame (runtime/nvn/weather.c)
void season_scene(void);  // a scene composite was drawn: the clock runs (weather_cycle_scene)
void season_save_read(const char *path, uint64_t addr, int64_t off, int64_t size);     // services/fs.c
void season_save_written(const char *path, uint64_t addr, int64_t off, int64_t size);
