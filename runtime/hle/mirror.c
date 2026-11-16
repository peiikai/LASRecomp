// Mirror mode (Gameplay tab, g_settings.mirror_mode): the world is shown mirrored left to right, the HUD and menus
// aren't. Each frame the game composites its finished scene into the window image with one unblended full-screen
// draw that samples the scene image (mirror_scene_size), then draws the HUD on top (blended); the renderers (runtime/nvn/renderer.m,
// vulkan/exec.c) mirror that composite (ShaderKey flip_x). While the mirrored world is on screen the stick and d-pad
// swap left and right (runtime/hle/input.c); the pause menu shows no composite (its backdrop is a small blurred copy),
// so there they don't.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "../settings.h"

uint64_t renderer_present_count(void);

static uint64_t g_world_frame = ~0ull;  // the mirrored scene was drawn
static uint64_t g_scene_frame = ~0ull;  // the scene composite was drawn (mirrored or not)

// Could an image of sw x sh be the scene, composited into a w x h window image? The game's dynamic resolution rendered
// busy scenes smaller (1536x864, 1280x720 into a 1920x1080 window image: the outdoors, D8; off now - tools/patches.py
// GAME_DYNRES_SITE - but LARECOMP_GAME_DYNRES=1 brings it back), so any size from half the
// window's width up, at its aspect ratio; the scratch draws into the window images sample a quarter of it or less.
// (Requiring the window's own size left those frames unmirrored: the world flipped back and forth, issue #66.)
bool mirror_scene_size(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) {
    uint64_t a = (uint64_t)sw * h, b = (uint64_t)sh * w;
    return sw * 2 >= w && sw <= w && sh <= h && (a > b ? a - b : b - a) * 100 <= (uint64_t)w * h;
}

// LARECOMP_LOG_SCENE=<n>: every n presented frames, log the size of the scene image the game composited into the window
// image (guest sizes, as the game allocated them: its main pass resolution) - checks the game's dynamic resolution.
#include <stdlib.h>
#include "../rt.h"
void scene_size_log(bool composite, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) {
    static long every = -1;
    static uint64_t last = ~0ull;
    if (every < 0) every = getenv("LARECOMP_LOG_SCENE") ? atol(getenv("LARECOMP_LOG_SCENE")) : 0;
    if (every <= 0 || !composite) return;
    uint64_t f = renderer_present_count();
    if (f / every == last) return;
    last = f / every;
    LOG("[scene] frame %llu scene %ux%u window image %ux%u\n", (unsigned long long)f, sw, sh, w, h);
}

// The renderer found the scene composite (composite): mirror it?
bool mirror_draw(bool composite) {
    if (composite) g_scene_frame = renderer_present_count();
    if (!g_settings.mirror_mode || !composite) return false;
    g_world_frame = renderer_present_count();
    return true;
}

// (render thread) the pause screen's backdrop - its blurred 192x108 copy of the scene, over the whole window - was
// drawn: a composite earlier in this frame is under it. The save screen keeps that backdrop but has the world drawn
// again behind it for a moment (the save file's picture), which alone looked like the world on screen: the D-pad
// items' cross flashed over the save screen.
static volatile uint64_t g_cover_frame = ~0ull;
void mirror_scene_covered(void) { g_cover_frame = renderer_present_count(); }

// (render thread) an nn::ui2d layout draw into the window image, untextured or of a copy of the screen (u: its vertex
// uniforms): one over the whole layout screen is a fill the game draws over the world and its HUD - the white behind
// "...INSTRUMENTS..." after an instrument is taken, fades, Game Over's blurred screen. The HUD's own panes stay shown under it, so the D-pad items' cross (drawn over
// everything) goes by how opaque it is. Uniforms: projection 0-15, model-view 16-27 (3x4), the corners' colours
// 116-131 (0-255), the quad 132-135 (width, height, left, top; layout units, y up).
static volatile uint64_t g_fill_frame = ~0ull;
static volatile int g_fill_a;
void mirror_layout_fill(const float *u, uint64_t size) {
    if (!u || size != 560 || u[15] != 1 || !(u[0] > 0) || !(u[5] > 0)) return;
    float l = u[134], t = u[135], r = l + u[132], b = t - u[133];
    float x0 = u[0] * (l * u[16] + u[19]) + u[3], x1 = u[0] * (r * u[16] + u[19]) + u[3];
    float y0 = u[5] * (t * u[21] + u[23]) + u[7], y1 = u[5] * (b * u[21] + u[23]) + u[7];
    if (!(x0 <= -0.999f && x1 >= 0.999f && y0 >= 0.999f && y1 <= -0.999f)) return;  // (clip space: the whole screen)
    float a = 255;
    for (int i = 0; i < 4; i++) a = fminf(a, u[119 + 4 * i]);
    uint64_t f = renderer_present_count();
    int ai = a > 0 ? (int)a : 0;
    if (g_fill_frame != f || ai > g_fill_a) g_fill_a = ai;
    g_fill_frame = f;
}
// how opaque the game's fill over the screen is, 0..255 (0: none this frame or the last)
int mirror_fill_alpha(void) {
    uint64_t f = g_fill_frame;
    return f != ~0ull && renderer_present_count() - f <= 1 ? g_fill_a : 0;
}

// The world is on screen (its composite drawn this frame or the last three: the render thread can be a frame or two
// behind the game's, and not covered by the pause screen) - not the pause menu, save screen, title or file select.
// (Frame pacing: runtime/nvn/pace.c.)
bool world_on_screen(void) {
    uint64_t f = g_scene_frame;
    return f != ~0ull && renderer_present_count() - f <= 3 && g_cover_frame != f;
}

// The stick / d-pad swap left and right: mirror mode and the mirrored world on screen (this frame or the last two).
bool mirror_input(void) {
    return g_settings.mirror_mode && g_world_frame != ~0ull && renderer_present_count() - g_world_frame <= 2;
}


// The in-world action prompt ("Lift", "Read", "Talk"... - Cm_HUD's L_Interact_00) sits at its target's spot on screen.
// tools/patches.py PROMPT_POS_SITE: the HUD writes the prompt pane's translation (x right of the centre) each time it
// places it; in mirror mode, with the mirrored world on screen, x is mirrored - the prompt itself reads as ever.
#include <string.h>
#include "../hle.h"
float rt_prompt_x(uint64_t pane, float x) {
    if (!mirror_input() || memcmp((const char *)(uintptr_t)(pane + 0xB0), "L_Interact_00", 14)) return x;
    return -x;
}
