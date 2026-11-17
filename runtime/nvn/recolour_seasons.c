// Cosmetics: the overworld's Leaves, Grass and Sand colours (Cosmetics tab, shared palette), live; the Season row fills
// all three (Spring = Default, Autumn = Orange / Gold / Gold, Winter = White, Cherry blossom = Pink / Default /
// Default). They recolour the overworld's foliage and sand, keeping the shading (recolour.c).
//   Leaves: tree canopies (Tree_01, the river trees' leaves, the Mysterious Woods' trees, fallen leaves, the telephone
//   trees' crowns, the talking trees' crowns), palm and banana leaves, bushes and plants (the liftable / cuttable bush
//   Grass_01 - also an enemy's bush disguise, so it still blends in -, field plants, the woods' ferns), the leaves
//   on the houses (their own decal maps: Sale's, Marin and Tarin's, Schule's, the Seashell Mansion's, the fairy
//   fountain's hedges). Not the houses' painted wood (Sale's green trim is in his house's atlas: it stays, as the
//   Animal Village's painted roofs do).
//   Grass: the ground's grass, tall grass Lawn_01 (the graveyard's too), grass tufts, the single ground tiles drawn
//   over it (ObjFieldTip*).
//   Sand: the beaches, Yarna Desert, the sandy south around the Face Shrine, the shore lines.
// The leaves flying off cut grass and bushes (effect sheets grass_00 / grass_01 / cut_lawn_00) get the same colour as
// what was cut. Only greens move (a hue range): flowers, trunks, dirt, rocks keep theirs.
#include <math.h>
#include <string.h>

#include "../settings.h"
#include "recolour.h"
#include "season.h"

// (S_BEACH: the pale beach sand; S_LIME: the lime / olive foliage maps; S_LAWN: the tall grass - its own window: the
// graveyard's is drawn dark teal; S_FERN: the Mysterious Woods' ferns, olive fronds beside a brown trunk in one map.
// Never tinted, for the snow on the ground only (renderer.m, snow_cover_check): S_FACE the face trees around Crazy
// Tracy's and the Tabahl Wasteland, S_ROCK the liftable rocks)
enum { S_TREE = 16, S_PALM, S_BUSH, S_GRASS, S_SAND, S_BEACH, S_MYST, S_LIME, S_LAWN, S_FERN, S_FACE, S_ROCK };

static bool ends_alb(const char *t) { size_t n = strlen(t); return n > 4 && !strcmp(t + n - 4, "_alb"); }
static bool pre(const char *t, const char *p) { return !strncmp(t, p, strlen(p)); }

static int field(int source, const char *t) {  // Field.bntx (the overworld's textures) and the foliage actors
    (void)source;
    if (!ends_alb(t)) return 0;
    // the Mysterious Woods' trees (olive, ~50 degrees: outside the greens' range): the whole canopy map moves
    if (pre(t, "MI_MysteriousTree_01_")) return S_MYST;
    if (pre(t, "MI_TabahlTree_01_")) return S_FACE;  // (the face trees: bark, nothing to tint)
    if (pre(t, "MI_Rock_01_")) return S_ROCK;
    if (pre(t, "MI_Tree_01_") || pre(t, "MI_d2_2d_ha_") || pre(t, "MI_BdTel")) return S_TREE;
    // lime and olive foliage (60-110 degrees: at or past the canopies' window's edge)
    if (pre(t, "MI_RiverTree_02_") || pre(t, "MI_Leaf_01_") || pre(t, "MI_TravellersTree_01_") ||
        pre(t, "MI_plants_01_") || pre(t, "MI_Osmunda_01_") || pre(t, "MI_LivelyWood_01_02_") ||
        pre(t, "MI_KCShutter_grass") || pre(t, "MI_SpeakTree_02_"))  // (SpeakTree_02: the talking trees' crowns)
        return S_LIME;
    if (pre(t, "MI_FlowersFl06_05_")) return S_FERN;
    if (pre(t, "MI_Lawn_01_")) return S_LAWN;
    if (pre(t, "MI_PalmTree_02_") || pre(t, "MI_BananaTree_02_")) return S_PALM;  // (BananaTree_01: trunk, bananas)
    // the houses' leaf decals (their own maps, beside the houses' painted atlases, which stay: Sale's green trim)
    if (pre(t, "MI_BdBanana_01_2_") || pre(t, "MI_BdMarinTarin_01_2_") || pre(t, "MI_BdSchule_02_") ||
        pre(t, "MI_BdSeashell_02_") || pre(t, "MI_FairyFountain_02_"))
        return S_BUSH;
    if (pre(t, "MI_Grass_01_") || pre(t, "MI_LivelyFlowers") || pre(t, "MI_FieldFlower")) return S_BUSH;
    if (pre(t, "MI_f_grass_a_") || pre(t, "MI_f_grass_c_") || pre(t, "MI_f_g_grass_") || pre(t, "MI_f_b_grass_kr") ||
        pre(t, "MI_TP_grass_") || pre(t, "MI_BdRichard_grass_"))
        return S_GRASS;
    // sand: the beaches (f_sand_c), Yarna Desert (f_sand_y), the sandy south (f_sand_a, the pale f_grass_b ground) and
    // the shore lines (f_mizube); not the paths (f_road, and f_sand_b which is the same orange), pebbles or rocks
    if (pre(t, "MI_f_sand_c_")) return S_BEACH;
    if (pre(t, "MI_f_sand_a_") || pre(t, "MI_f_sand_y_") || pre(t, "MI_f_mizube_") ||
        !strcmp(t, "MI_f_grass_b_01_alb") || !strcmp(t, "MI_f_grass_b_01b_alb") || pre(t, "MI_f_grass_b_02_") ||
        pre(t, "MI_f_grass_b_tunagi_"))
        return S_SAND;
    return 0;
}

static int effect(int source, const char *t) {  // the leaves of cut grass / bushes
    (void)source;
    if (!strcmp(t, "grass_00") || !strcmp(t, "cut_lawn_00")) return S_GRASS;
    if (!strcmp(t, "grass_01")) return S_BUSH;
    return 0;
}

static const RecolourSource k_sources[] = {
    {"region_common/map/Field.bntx", field},
    {"region_common/actor/ObjTree.bfres", field},
    {"region_common/actor/ObjGrass.bfres", field},
    {"region_common/actor/ObjLawn.bfres", field},
    {"region_common/actor/ObjPalmTree.bfres", field},
    {"region_common/actor/ObjMysteriousTree.bfres", field},
    {"region_common/actor/ObjRock.bfres", field},
    {"region_common/actor/ObjTabahlTree.bfres", field},
    {"region_common/actor/ObjSpeakTree.bfres", field},
    // (single ground tiles placed over the level's ground, with their own copies of its maps: they stayed green under
    // an autumn ground, issue #577)
    {"region_common/actor/ObjFieldTipKanalet.bfres", field},
    {"region_common/actor/ObjFieldTipMaze.bfres", field},
    {"region_common/actor/ObjFieldTipMysterious.bfres", field},
    {"region_common/actor/ObjFieldTipUkuku.bfres", field},
    {"region_common/effect/Common.ptcl", effect},
    {"region_common/effect/Player.ptcl", effect},
    {"region_common/effect/Object.ptcl", effect},
};

// The rows' colour is a target for the greens' typical colour as drawn (hue in degrees, saturation, value): leaves
// (140, 0.8, 0.58: the canopies are mostly in their own shade, so they're lifted more), grass (100, 0.65, 0.8). Only
// greens move: hues within ~72 degrees of the leaves' 140 (their shaded parts reach ~180) / ~61 of the grass' 100
// (the dirt paths, ~35, stay out); their own variety is kept at 35%, around that typical hue.
typedef struct {
    int row;                 // 0 leaves, 1 grass, 2 sand
    float h, s, v;           // the typical colour as drawn (degrees, 0..1, 0..1)
    float dh;                // a hue offset on top of the mapping (degrees)
    float centre, width, min_sat;  // the hue window (degrees, turns, saturation)
} Geo;

static bool geo(int cls, Geo *g) {
    switch (cls) {
    case S_FACE: case S_ROCK: return false;
    // (sand reads 30-50 degrees: only that range moves; typical: the desert / south (38, 0.8, 0.8), the pale beach
    // (45, 0.25, 0.9))
    case S_SAND: *g = (Geo){2, 38, 0.8f, 0.8f, 0, 38, 0.1f, 0.12f}; return true;
    case S_BEACH: *g = (Geo){2, 45, 0.25f, 0.9f, 0, 45, 0.1f, 0.05f}; return true;
    case S_MYST: *g = (Geo){0, 55, 0.8f, 0.45f, 0, 55, 0.5f, 0.08f}; return true;  // (Leaves; typical: 55, 0.8, 0.45)
    // (the tall grass: hues 30-210, around 120 - its blades have nothing else)
    case S_GRASS: *g = (Geo){1, 100, 0.65f, 0.8f, 0, 100, 0.17f, 0.12f}; return true;
    case S_LAWN: *g = (Geo){1, 100, 0.65f, 0.8f, -20, 120, 0.25f, 0.12f}; return true;
    // (Leaves; drawn in the woods' blue light the fronds read 140-200 degrees, typical (155, 0.5, 0.4); the trunk stays
    // under 80)
    case S_FERN: *g = (Geo){0, 155, 0.5f, 0.4f, 0, 155, 0.2f, 0.12f}; return true;
    // (Leaves; typical: 95, 0.7, 0.6; hues within ~72 degrees of 110 move)
    case S_LIME: *g = (Geo){0, 95, 0.7f, 0.6f, -15, 110, 0.2f, 0.12f}; return true;
    default: *g = (Geo){0, 140, 0.8f, 0.58f, 0, 140, 0.2f, 0.12f}; return true;
    }
}

static uint32_t hsv_rgb(float h, float s, float v) {  // (h in degrees)
    float c = v * s, hh = h / 60, x = c * (1 - fabsf(fmodf(hh, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    int i = (int)hh % 6;
    if (i == 0) r = c, g = x; else if (i == 1) r = x, g = c; else if (i == 2) g = c, b = x;
    else if (i == 3) g = x, b = c; else if (i == 4) r = x, b = c; else r = c, b = x;
    return (uint32_t)((r + m) * 255 + 0.5f) << 16 | (uint32_t)((g + m) * 255 + 0.5f) << 8 | (uint32_t)((b + m) * 255 + 0.5f);
}

// The colour row `c` aims at (Default: the typical colour itself) and how much of the hues' own variety stays (Default:
// all of it). False: Default.
static bool target(const Geo *g, const CosColour *c, uint32_t *rgb, float *spread) {
    static const int slots[3] = {COL_SLOT_LEAVES, COL_SLOT_GRASS, COL_SLOT_SAND};
    if (col_target(c, slots[g->row], rgb)) { *spread = 0.35f; return true; }
    *rgb = hsv_rgb(g->h, g->s, g->v), *spread = 1;
    return false;
}

static bool tint(int cls, float t[8]) {
    Geo g;
    if (!geo(cls, &g)) return false;
    uint32_t rgb;
    float spread, on, sh, sf, vf;
    int from, to;
    float k;
    if (season_blend(&from, &to, &k)) {
        // Season: Random - one season fading into the next: the two targets mixed (in RGB: autumn's orange pales on
        // its way to winter's white, spring's pink greys a little on its way to the game's green - mixing the hues
        // instead swung through other colours)
        CosColour ca = {g_season_preset[from][g.row], 0}, cb = {g_season_preset[to][g.row], 0};
        uint32_t a, b;
        float pa, pb;
        bool oa = target(&g, &ca, &a, &pa), ob = target(&g, &cb, &b, &pb);
        if (!oa && !ob) return false;
        rgb = 0;
        for (int i = 0; i < 24; i += 8) {
            float x = (a >> i & 255) + ((float)(b >> i & 255) - (a >> i & 255)) * k;
            rgb |= (uint32_t)(x + 0.5f) << i;
        }
        spread = pa + (pb - pa) * k, on = (oa ? 1 - k : 0) + (ob ? k : 0);
    } else {
        const CosColour *rows[3] = {&g_settings.leaves, &g_settings.grass, &g_settings.sand};
        if (!target(&g, rows[g.row], &rgb, &spread)) return false;
        on = 1;
    }
    col_mapping(g.h / 360.0f, g.s, g.v, rgb, &sh, &sf, &vf);
    recolour_pack(t, sh + on * g.dh / 360.0f, sf, vf, recolour_deg(g.centre), g.width, g.min_sat, spread);
    return true;
}

const RecolourOption g_recolour_seasons = {"leaves / grass / sand", k_sources, (int)(sizeof k_sources / sizeof *k_sources), S_TREE,
                                           S_ROCK, tint};
