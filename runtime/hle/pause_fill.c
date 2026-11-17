// The pause screen (Items, Map, System) on a window that isn't 16:9, with Aspect ratio = Unlocked: its backdrop fills
// the window instead of the centred 16:9 box, with no bands of game world around it (iPad at 4:3, ultrawide).
//
// The screen first draws the blurred scene (a 192x108 copy) as a composite over the whole window (renderer.m /
// vulkan/exec.c), which marks the frame here. Its layout panes then go into the 16:9 box:
// - the frosted layers sampling that window-shaped copy cover the box: they are drawn over the window as well, so they
//   line up with the blurred scene under them (in the box they showed the window's whole width squeezed);
// - the panes reaching three of the box's edges (the paper frame, the window pane that darkens its border) are drawn
//   with the window-wide viewport and the projection divided by the stretch (as runtime/hle/hud_corners.c does for the
//   HUD: an unmoved point lands on the same pixels), their edges at the box's border moved out to the window's and
//   their texture coordinates carried on at the same rate. Inside the box nothing changes;
// - everything else (items, text, the gold lines) stays in the box.
// In a boxed room (interiors, dungeons) the frame is shown whole rather than cropped to 16:9, its bars cleared after
// the blurred scene (hud_corners.c's bars).
#include <stdbool.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

extern uint64_t renderer_exec_frame(void);
extern int game_in_play(void);

static volatile uint64_t g_frame = ~0ull;  // the frame being translated drew the pause screen's blurred scene

// (and the next two: the screen's last fade-out frame is drawn over the live world, without the blurred copy, and its
// backdrop showed boxed for that frame)
bool pause_fill_frame(uint64_t frame) { return g_frame != ~0ull && frame >= g_frame && frame - g_frame <= 2; }

// (render thread) a composite sampling a w x h texture: the pause screen's blurred scene?
bool pause_fill_backdrop(int w, int h) {
    if (w != 192 || h != 108 || g_settings.aspect != ASPECT_UNLOCKED || ENV("LARECOMP_NO_PAUSE_FILL") || !game_in_play())
        return false;
    g_frame = renderer_exec_frame();
    return true;
}

// A layout pane of a pause frame sampling a window-shaped image (the blurred scene: its whole width is the window's)
// and covering the box: drawn over the window, as the blurred scene itself.
bool pause_fill_scene_pane(bool reshaped, const float *u, uint64_t size) {
    extern bool widescreen_ui_pane_partial(const float *u, uint64_t size);
    return reshaped && u && size == 560 && pause_fill_frame(renderer_exec_frame()) && !widescreen_ui_pane_partial(u, size);
}

// A window pane's frame coordinate along one axis at corner share a (0 at the left / top) of a rect size long, as the
// layout vertex shader makes it: stretched (0..1) or per texel (tile, texel size tau), aligned to the far edge, flipped.
static float frame_coord(float a, float size, float tau, bool tile, bool far, bool flip) {
    float g = (tile ? size * tau * a : a) + (far ? 1 - size * tau : 0);
    return flip ? 1 - g : g;
}

// The rect grown by shares e0 (left / top) and e1 of its size: old coordinate = *ka * new coordinate + *ko.
static void frame_fit(float tau, float size, float e0, float e1, bool tile, bool far, bool flip, float *ka, float *ko) {
    float n = 1 + e0 + e1, size2 = size * n;
    float o0 = frame_coord(0, size, tau, tile, far, flip), o1 = frame_coord(1, size, tau, tile, far, flip);
    float n0 = frame_coord(e0 / n, size2, tau, tile, far, flip), n1 = frame_coord((1 + e0) / n, size2, tau, tile, far, flip);
    *ka = n1 != n0 ? (o1 - o0) / (n1 - n0) : 1;
    *ko = o0 - *ka * n0;
}

// A copy of a layout pane's vertex uniforms reaching the window's edges (stretch k, ky), NULL when this isn't a pause
// frame or the pane doesn't reach the box's edge. Uniforms (560 bytes): projection (floats 0-15), the pane's
// model-view rows (16-27), per texture (3) the corners' texture coordinates (56 + 16 i: left top, right top, left
// bottom, right bottom; 4 floats each), the pane's rect (132-135: width, height, left, top; y up).
const void *pause_fill_ubo(const float *u, uint64_t size, float k, float ky) {
    if (!u || size != 560 || (k <= 1.001f && ky <= 1.001f) || !pause_fill_frame(renderer_exec_frame())) return NULL;
    if (u[15] != 1 || u[12] != 0 || u[13] != 0 || !(u[0] > 0) || u[5] == 0) return NULL;  // (orthographic projection)
    if (u[1] != 0 || u[4] != 0 || u[17] != 0 || u[20] != 0) return NULL;                    // (not rotated)
    float w = u[132], h = u[133], l = u[134], t = u[135];
    if (!(w > 0) || !(h > 0) || !(w < 1e5f) || !(h < 1e5f)) return NULL;
    float ax = u[0] * u[16], ay = u[5] * u[21];  // ndc per local unit
    if (!(ax > 0) || ay == 0) return NULL;
    float xl = ax * l + u[0] * u[19] + u[3], xr = xl + ax * w;
    float yt = ay * t + u[5] * u[23] + u[7], yb = yt - ay * h;  // (ay < 0: the top is the lower ndc)
    float ylo = ay > 0 ? yb : yt, yhi = ay > 0 ? yt : yb;
    // an edge on the box's border moves to the window's (ndc +-k after the rescale); only backdrop panes, reaching three
    // edges of the box (the window pane's two pieces: the frame's right end reaches three), so a piece sliding in at an
    // edge isn't smeared
    bool tl = xl <= -0.99f, tr = xr >= 0.99f, tlo = ylo <= -0.99f, thi = yhi >= 0.99f;
    if (tl + tr + tlo + thi < 3) return NULL;
    float el = 0, er = 0, elo = 0, ehi = 0;  // in ndc of the box
    if (tl) el = xl + k + 0.01f;
    if (tr) er = k + 0.01f - xr;
    if (tlo) elo = ylo + ky + 0.01f;
    if (thi) ehi = ky + 0.01f - yhi;
    if (el <= 0.001f && er <= 0.001f && elo <= 0.001f && ehi <= 0.001f) return NULL;
    float fay = ay > 0 ? ay : -ay;
    float et = (ay > 0 ? ehi : elo) / fay, eb = (ay > 0 ? elo : ehi) / fay;  // local units, top and bottom
    float fl = el / ax / w, fr = er / ax / w, ft = et / h, fb = eb / h;     // shares of the rect
    static float ring[1024][140];
    static unsigned at;
    float *c = ring[at++ & 1023];
    memcpy(c, u, size);
    for (int i = 0; i < 4; i++) c[i] /= k, c[4 + i] /= ky;
    c[132] = w * (1 + fl + fr), c[133] = h * (1 + ft + fb), c[134] = l - w * fl, c[135] = t + h * ft;
    uint32_t mode[3], flags;  // (bit 0 of flags: a window pane's frame, its texture coordinates made from the corner)
    memcpy(mode, u + 136, sizeof mode);
    memcpy(&flags, u + 139, 4);
    for (int i = 0; i < 3; i++) {
        if (mode[i]) continue;  // (coordinates from the position through a matrix: they carry on by themselves)
        if (flags & 1) {        // the texture SRT (28 + 8 i: two rows) gets the change from the new corners to the old
            if (flags & 0x10000) return NULL;  // (rotated frame: not seen on the pause screen)
            float as, os, at, ot;
            frame_fit(u[112], w, fl, fr, flags & 0x10, flags & 0x4, flags & 0x20000, &as, &os);
            frame_fit(u[113], h, ft, fb, flags & 0x20, flags & 0x8, flags & 0x40000, &at, &ot);
            float *m = c + 28 + 8 * i;
            for (int row = 0; row < 2; row++, m += 4)
                m[3] += m[0] * os + m[1] * ot, m[0] *= as, m[1] *= at;
            continue;
        }
        const float *s = u + 56 + 16 * i;  // the corners' texture coordinates (left top, right top, left bottom, right
        float *d = c + 56 + 16 * i;        // bottom), carried on (bilinear)
        for (int corner = 0; corner < 4; corner++) {
            float a = corner & 1 ? 1 + fr : -fl, b = corner & 2 ? 1 + fb : -ft;
            for (int j = 0; j < 4; j++)
                d[4 * corner + j] = s[j] + a * (s[4 + j] - s[j]) + b * (s[8 + j] - s[j]) +
                                    a * b * (s[12 + j] - s[8 + j] - s[4 + j] + s[j]);
        }
    }
    return c;
}
