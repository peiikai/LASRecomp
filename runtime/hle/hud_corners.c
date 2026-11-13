// Phones and tablets: with Aspect ratio = Unlocked and a window wider or taller than 16:9, the game's HUD goes to the real
// screen corners instead of the centred 16:9 box, in every room so it never jumps at a door; and optionally its X / Y item
// buttons are hidden while the on-screen controls (which show those items) are up.
//
// Game thread (rt_hud_corners_tick, from the HUD's state update, HUD_HEARTS_SITE, x0 = the HUD): each group pane under
// Cm_HUD's N_HUD_00 gets translate x (+0x30) = authored x -/+ the shift, then the game's own "matrix dirty" bit
// (+0x58 |= 0x10, as at PROMPT_POS_SITE). Absolute writes: the same each frame, the authored x kept from the first.
//
// Render thread: layout draws go into the 16:9 box (renderer.m / vulkan/exec.c). Those draws get the window-wide viewport
// (centred stretch k) and a copy of their uniforms with the projection's x row divided by k: an unmoved pane lands on the
// same pixels, a moved one in the bars. Boxed rooms are cropped to 16:9 at present, so while the HUD is there the
// bars are cleared after the scene composite and the frame is shown whole (widescreen_present_crop).
// The render-thread part also serves the pause screen's backdrop on every platform (runtime/hle/pause_fill.c).
#include "../game_addresses.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../settings.h"

float widescreen_factor(void);
bool world_on_screen(void);
extern int game_in_play(void);

static volatile float g_left, g_right, g_up;  // layout units (1280 x 720 box) the HUD's groups moved by

float hud_corners_left(void) { return g_left; }
float hud_corners_right(void) { return g_right; }
float hud_corners_up(void) { return g_up; }

static volatile bool g_active;  // (game thread: the HUD's groups are moved)
bool pause_fill_frame(uint64_t frame);  // runtime/hle/pause_fill.c: the pause screen fills the window too
extern uint64_t renderer_exec_frame(void);

#if PLAT_TOUCH

bool touch_visible(void);
void menu_safe_insets(float *l, float *r);  // share of the window's width

// (direct children of N_HUD_00 only: sub-layouts reuse these names)
// N_Key_00 (keys, boss key, golden leaves) has its y set by the game's animations every frame (Cm_HUD_KeyUpDown:
// 314 under one row of hearts, 288 under two; DirectMapIn / Out: 268), so a write to it is lost: its x is moved here,
// its y through its children (not animated, in its scaled units).
enum { LEFT = -1, KEEP = 0, RIGHT = 1 };
static const struct { const char *name; int side; bool xy, kids_y; } kPanes[] = {
    {"N_Life_00", LEFT, false},  {"N_Key_00", LEFT, false, true},   {"N_Item_00", RIGHT, true},
    {"N_Item_01", RIGHT, true},  {"N_SetItem_00", RIGHT, true},    {"N_Rupee_00", RIGHT, false},
    {"N_ShellRadar_00", RIGHT, false},
};
#define NPANES (sizeof kPanes / sizeof kPanes[0])
static uint64_t g_hud, g_pane[NPANES], g_retry;
static int g_tries;
static float g_authored[NPANES], g_authored_y[NPANES];
static bool g_moved, g_hid[NPANES];
enum { MAX_KIDS = 32 };
static uint64_t g_kid[MAX_KIDS], g_kid_of;
static float g_kid_y[MAX_KIDS], g_kid_set[MAX_KIDS];  // (the game's y; the y last written here)
static unsigned g_nkids;

static bool name_is(uint64_t p, const char *n) { return !strcmp((const char *)(uintptr_t)(p + 0xB0), n); }
static uint64_t g_ra[256], g_rs[256];
static int g_nr;
static bool mapped(uint64_t p) {  // (a pointer found by scanning, or a stale pane's parent: any value at all)
    for (int k = 0; k < g_nr; k++)
        if (p >= g_ra[k] && p + 0x800 > p && p + 0x800 <= g_ra[k] + g_rs[k]) return true;
    return false;
}
static bool child_of_hud(uint64_t p) {
    uint64_t up = LD64(p + 0x18);
    return mapped(up) && name_is(up, "N_HUD_00");
}

// breadth first from the HUD over pointers into guest data, as runtime/ui/tracker_overlay.mm finds its panes
static void find_panes(uint64_t hud) {
    enum { MAX_SEEN = 1 << 15, TAB = 1 << 17 };
    static uint64_t q[MAX_SEEN], tab[TAB];
    memset(tab, 0, sizeof tab);
    unsigned n = 1, found = 0;
    q[0] = hud;
    memset(g_pane, 0, sizeof g_pane);
    for (unsigned i = 0; i < n && found < NPANES; i++)
        for (uint32_t off = 0; off < 0x800 && found < NPANES; off += 8) {
            uint64_t p = LD64(q[i] + off);
            if (!mapped(p)) continue;
            unsigned h = (unsigned)(p * 0x9E3779B1u >> 15) & (TAB - 1);
            while (tab[h] && tab[h] != p) h = (h + 1) & (TAB - 1);
            if (tab[h] || n >= MAX_SEEN) continue;
            tab[h] = p, q[n++] = p;
            if (*(const char *)(uintptr_t)(p + 0xB0) != 'N' || !child_of_hud(p)) continue;
            for (unsigned k = 0; k < NPANES; k++)
                if (!g_pane[k] && name_is(p, kPanes[k].name)) {
                    g_pane[k] = p, g_authored[k] = *(const float *)(uintptr_t)(p + 0x30), g_hid[k] = false;
                    g_authored_y[k] = *(const float *)(uintptr_t)(p + 0x34);
                    found++;
                }
        }
    LOG("[hud] corners: %u of %u panes found from %llx (%u objects)\n", found, (unsigned)NPANES, (unsigned long long)hud, n);
}

// the children of pane p (nn::ui2d::Pane: link node at +0x8, parent +0x18, child list root at +0x20, next at +0x28)
static void find_kids(uint64_t p) {
    g_nkids = 0, g_kid_of = p;
    uint64_t root = p + 0x20;
    for (uint64_t n = LD64(root + 8); n != root && g_nkids < MAX_KIDS; n = LD64(n + 8)) {
        uint64_t k = n - 8;
        if (!mapped(k) || LD64(k + 0x18) != p) { g_nkids = 0; break; }
        g_kid[g_nkids] = k, g_kid_y[g_nkids] = g_kid_set[g_nkids] = *(const float *)(uintptr_t)(k + 0x34), g_nkids++;
    }
    LOG("[hud] corners: %u children of %s\n", g_nkids, (const char *)(uintptr_t)(p + 0xB0));
}

// each child's y = the game's y + up (the game may place them itself: a y not ours is its new one)
static void move_kids(float up) {
    for (unsigned j = 0; j < g_nkids; j++) {
        uint64_t k = g_kid[j];
        if (LD64(k + 0x18) != g_kid_of) continue;  // (gone with a stale parent, found again with it)
        float *y = (float *)(uintptr_t)(k + 0x34);
        if (*y != g_kid_set[j]) g_kid_y[j] = *y;
        float want = g_kid_y[j] + up;
        if (*y == want) continue;
        *y = g_kid_set[j] = want;
        *(uint8_t *)(uintptr_t)(k + 0x58) |= 0x10;
    }
}

static void set_xy(unsigned i, float x, float y) {
    uint64_t p = g_pane[i];
    if (*(const float *)(uintptr_t)(p + 0x30) == x && *(const float *)(uintptr_t)(p + 0x34) == y) return;
    *(float *)(uintptr_t)(p + 0x30) = x;
    *(float *)(uintptr_t)(p + 0x34) = y;
    *(uint8_t *)(uintptr_t)(p + 0x58) |= 0x10;
}

void rt_hud_corners_tick(uint64_t hud) {
    float f = widescreen_factor();
    bool in = game_in_play() && world_on_screen();
    bool want = in && g_settings.aspect == ASPECT_UNLOCKED && (f > 1.01f || f < 0.99f) && !ENV("LARECOMP_NO_HUD_CORNERS");
    bool hide = in && (g_settings.touch_hide_xy_hud || ENV("LARECOMP_HIDE_XY_HUD")) && touch_visible();
    if (!want && !hide && !g_moved) {
        bool any = false;
        for (unsigned i = 0; i < NPANES; i++) any |= g_hid[i];
        if (!any) { g_active = false, g_left = g_right = 0; return; }
    }
    extern uint64_t renderer_present_count(void);
    extern int kernel_data_regions(uint64_t *a, uint64_t *s, int max);
    g_nr = kernel_data_regions(g_ra, g_rs, 256);
    bool stale = false, missing = false;  // (a pane gone: its memory may be something else now, don't write to it)
    for (unsigned i = 0; i < NPANES; i++) {
        missing |= !g_pane[i];
        stale |= g_pane[i] && !(name_is(g_pane[i], kPanes[i].name) && child_of_hud(g_pane[i]));
    }
    if (hud != g_hud) g_tries = 0, g_kid_of = 0;  // (a pane found again on the same HUD keeps its children's state)
    if (hud != g_hud || stale || (missing && g_tries < 5)) {  // (a new HUD, each screen load; not all there yet: once a second)
        if (renderer_present_count() < g_retry) return;
        g_hud = hud, g_retry = renderer_present_count() + 60, g_moved = false, g_tries++;
        find_panes(hud);
    }
    float l = 0, r = 0, up = 0;
    if (want && f < 1) up = 360 * (1 / f - 1);  // (a tablet: the band's spare height above it)
    else if (want) {
        float il, ir;
        menu_safe_insets(&il, &ir);  // (the camera cutout: on one side only in landscape)
        float span = 1280 * f;       // layout units across the window
        l = 640 * (f - 1) - il * span, r = 640 * (f - 1) - ir * span;
        if (l < 0) l = 0;
        if (r < 0) r = 0;
    }
    for (unsigned i = 0; i < NPANES; i++) {
        if (!g_pane[i]) continue;
        float y = g_authored_y[i] + up;
        if (kPanes[i].kids_y) {
            if (g_kid_of != g_pane[i]) find_kids(g_pane[i]);
            float sy = *(const float *)(uintptr_t)(g_pane[i] + 0x4C);  // (its scale, 0.75: the children's units)
            move_kids(sy > 0.01f ? up / sy : up);
            y = *(const float *)(uintptr_t)(g_pane[i] + 0x34);
        }
        set_xy(i, g_authored[i] + (kPanes[i].side == LEFT ? -l : kPanes[i].side == RIGHT ? r : 0), y);
        if (!kPanes[i].xy) continue;
        uint8_t *fl = (uint8_t *)(uintptr_t)(g_pane[i] + 0x58);
        if (hide && (*fl & 1)) *fl &= ~1, g_hid[i] = true;  // (bit 0: visible; the game may set it again)
        else if (!hide && g_hid[i]) *fl |= 1, g_hid[i] = false;
    }
    g_moved = want, g_active = want, g_left = l, g_right = r, g_up = up;
}

#else

void rt_hud_corners_tick(uint64_t hud) { (void)hud; }

#endif

// ---- render thread
static uint64_t g_bars_frame = ~0ull, g_crop_frame = ~0ull;
static float g_crop_u0, g_crop_u1, g_crop_v0, g_crop_v1;

bool hud_corners_active(void) { return g_active || pause_fill_frame(renderer_exec_frame()); }

// A copy of a layout draw's vertex uniforms for the window-wide viewport (stretch k), NULL when this isn't one.
const void *hud_corners_ubo(const float *u, uint64_t size, float k, float ky) {
    if (!g_active || !u || (size != 560 && size != 128) || (k <= 1.001f && ky <= 1.001f)) return NULL;
    if (u[15] != 1 || u[12] != 0 || u[13] != 0 || !(u[0] > 0) || u[5] == 0) return NULL;  // (orthographic projection)
    static float ring[4096][140];
    static unsigned at;
    float *c = ring[at++ & 4095];
    memcpy(c, u, size);  // (560: panes; 128: their text)
    for (int i = 0; i < 4; i++) c[i] /= k, c[4 + i] /= ky;
    return c;
}

void hud_corners_note_crop(uint64_t frame, float u0, float u1, float v0, float v1) {
    g_crop_frame = frame, g_crop_u0 = u0, g_crop_u1 = u1, g_crop_v0 = v0, g_crop_v1 = v1;
}
bool hud_corners_shown_whole(uint64_t frame) { return (g_active || pause_fill_frame(frame)) && g_bars_frame == frame; }

// (the scene composite is drawn) the bars to clear before the HUD, in the target's pixels: false when the last frame had none
bool hud_corners_bars(uint64_t frame, int w, int h, int bar[4]) {
    if (!(g_active || pause_fill_frame(frame)) || g_crop_frame + 2 < frame || g_crop_frame > frame) return false;
    g_bars_frame = frame;
    bar[0] = (int)(w * g_crop_u0), bar[1] = (int)(w * g_crop_u1 + 0.999f);
    bar[2] = (int)(h * g_crop_v0), bar[3] = (int)(h * g_crop_v1 + 0.999f);
    return bar[0] > 0 || bar[1] < w || bar[2] > 0 || bar[3] < h;
}
