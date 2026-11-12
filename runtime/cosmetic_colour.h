// The Cosmetics tab's colour rows (tunic, hearts, hair, skin, shield, leaves, grass, sand, water, fences, walls,
// houses) share one palette: Default (the game's own), thirteen named colours, Random and Custom (any colour, from a
// colour picker; saved as #RRGGBB). Grass and walls leave Black out (col_offered). Each row recolours its thing so that its typical colour becomes the chosen one,
// keeping the shading (col_mapping).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { COL_DEFAULT = 0, COL_BLACK = 11, COL_GRAY = 12, COL_RANDOM = 14, COL_CUSTOM = 15, N_COL = 16 };  // (1..13: the
                                                                                                    // named colours)

typedef struct CosColour {
    int pick;      // COL_DEFAULT, a named colour, COL_RANDOM or COL_CUSTOM
    uint32_t rgb;  // the Custom colour, 0xRRGGBB
} CosColour;

// Which row (Random picks a colour per row, again whenever the row is set to Random)
enum { COL_SLOT_TUNIC, COL_SLOT_HEARTS, COL_SLOT_HAIR, COL_SLOT_SKIN, COL_SLOT_SHIELD, COL_SLOT_LEAVES, COL_SLOT_GRASS,
       COL_SLOT_SAND, COL_SLOT_WATER, COL_SLOT_FENCES, COL_SLOT_WALLS, COL_SLOT_HOUSES,
       COL_SLOTS };  // (new rows at the end: a seed's own colours stay per slot)

extern const char *const g_col_names[N_COL];
extern const uint32_t g_col_rgb[N_COL];  // the named colours' targets (0 for Default / Random / Custom)

// larecomp.ini value -> c: a palette name, #RRGGBB (Custom), or an older version's name (Yellow, Sky, Lime, Blond,
// ...: the nearest palette colour); anything else: Default.
void col_parse(CosColour *c, const char *val);
// c -> its larecomp.ini value (name, or #RRGGBB for Custom)
void col_format(const CosColour *c, char *out, size_t n);
// The randomizer seed's own cosmetics (seed option "Random cosmetics", runtime/cosmetics_seed.c): looks for a newly
// played seed's set and copies it into the rows once. NULL until set.
extern void (*g_col_seed_check)(void);
// Whether row `slot` offers palette entry `pick`: every row but grass and walls, whose Black reads as holes (user
// 2026-11-11: Gray, added for them, is on every row)
bool col_offered(int pick, int slot);
// A pick the row doesn't offer (an older larecomp.ini's grass = Black) -> the one it offers in its place
void col_fix(CosColour *c, int slot);
// A random named colour the row offers
int col_random_pick(int slot, unsigned r);
// The colour row `slot` should show now (0xRRGGBB); false: Default (the game's own look).
bool col_target(const CosColour *c, int slot, uint32_t *rgb);
// HSV (hue in turns, saturation, value: 0..1) of 0xRRGGBB
void col_hsv(uint32_t rgb, float *h, float *s, float *v);
// The HSV change that takes a thing whose typical colour is (src_h, src_s, src_v) to `dst` (0xRRGGBB): hue shift
// (turns), saturation and value factors. A grey / white / black target keeps the hue and takes the saturation away.
void col_mapping(float src_h, float src_s, float src_v, uint32_t dst, float *shift, float *sat, float *val);
