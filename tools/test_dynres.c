// Dynamic resolution's controller (runtime/nvn/output_scale.c) driven by a simulated GPU: no game files, no window.
// Build + run: tools/test_dynres.py (prints each case's factor path; exits non-zero when a case misbehaves).
// The GPU: the game's part G = (a + b f^2) x scene x noise at factor f (a: what doesn't scale with the resolution), the
// window step W (upscale / SMAA / menu: the window's size only). A frame takes G + W of GPU; with vsync and the
// frames waiting for the display (fifo, as on Windows' Intel driver) it lasts the next multiple of 16.7 ms, with
// mailbox max(16.7, G + W). Cases from an Intel UHD 770 (Field, Dynamic): G and W as measured at the factor it sat at.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/settings.h"
#include "../runtime/nvn/output_scale.h"

Settings g_settings = {.res_mode = RES_DYNAMIC, .dyn_min = 540};  // (Minimum resolution: each case sets it, run())
bool g_gpu_integrated = true;
void pace_hitch(const char *why) { (void)why; }  // (output_scale.c's render scale changes: nvn/pace.c)
void pace_settings_changed(const char *what) { (void)what; }  // (output_scale.c's setting watch: nvn/pace.c)
static int g_t30;  // (the case's Internal FPS target is 30)
bool pace_target_30(void) { return g_t30; }  // (output_scale.c's Internal FPS target: nvn/pace.c)
static bool g_menu;  // (the case's menu is open: wmenu_at .. wmenu_until)
bool menu_is_open(void) { return g_menu; }  // (output_scale.c holds Dynamic while the menu is open: runtime/ui/menu.mm)
static int g_pace_slow;  // (pacer cases: the pacer holds the game at 30 - nvn/pace.c pace_is_slow)
bool pace_is_slow(void) { return g_pace_slow; }
static int g_pace_try;  // (pacer cases: Dynamic asked the frame pacer to try 60 - nvn/pace.c pace_try_60)
void pace_try_60(void) { g_pace_try = 1; }

static unsigned g_rng = 1;
static double rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return ((g_rng >> 8) & 0xffff) / 65535.0; }

typedef struct {
    const char *name;
    double a, b, w;     // G = a + b f^2, W
    int fifo;           // frames wait for the display
    double light_at;    // > 0: the scene gets lighter (x0.6) from then on (s)
    double secs;
    double wobble;      // the scene's load swings +-this over 20 s (0: 3%; frame to frame +-2%: the floor's 30.6 fps)
    double heavy_at, heavy, heavy_until;  // > 0: G x heavy from heavy_at (s) until heavy_until (0: the end)
    double wander;      // > 0: the scene's load wanders at random (walking around: +-this typical, over a few seconds)
    double lines;       // the window's height (0: 2160, 4K); the floor is min_lines of it (540: 0.50 at 1080p, 0.25 at 4K)
    double clock_slow;  // > 0: the GPU clocks down while mostly idle (an iGPU): its times x up to this, at under 60% busy
    double resize_at, lines2;  // > 0: the window's height becomes lines2 then (fullscreen <-> windowed): G's scaling part
                               // and W follow the window's area
    int min_lines;      // Minimum resolution (0: 540, the default; 360 or 720)
    int t30;            // Internal FPS target 30: frames paced to whole 33.3 ms
    double cpu;         // > 0: CPU time per frame that doesn't overlap the GPU's (ms; the frame takes cpu + G + W)
    double stall_at, stall_until, stall_ms;  // > 0: frames that long later from stall_at to stall_until (s): shaders
                                             // compiling after a warp, the GPU idle
    double wmenu_at, wmenu_until, wmenu;  // > 0: the window step takes wmenu ms from wmenu_at to wmenu_until (s): the
                                          // menu open
    double paced;       // > 0: the GPU's clock follows the load (Apple GPUs: iPad Pro M4): it clocks down until G + W is
                        // about 90% of the frame, down to 1 / paced of its full clock - its times x up to `paced`
    double w1;          // > 0: the window step at 1.00 (MetalFX only runs below 1.00: W is w there)
    int start_paced;    // the controller counts the GPU as paced from the start (Android / iOS: LARECOMP_DYN_PACED)
    int pacer;          // the frame pacer (nvn/pace.c) as well: 12 of the last 60 frames late hold the game at 30 (whole
                        // 33.3 ms frames); it goes back to 60 when the GPU's time (G + W, as measured: a lowered clock
                        // counts) is under 13 ms for 60 frames in a row, or at once when Dynamic asks (pace_try_60) -
                        // either way with a second's grace before frames count again
    int miss_every;     // > 0: one frame in this many misses a vsync (a CPU-side hitch: 35 ms, 59.5 fps windows)
    double jitter;      // > 0: about once a second a frame's period is this much longer and the next's that much shorter
                        // (Android: present times jitter, frames of 20-23 ms with no vsync missed - max 22 ms every 2 s)
    double wf;          // > 0: W grows with the factor: W = w * (1 + wf * (f - 0.5)) (RP5 FSR 1: 4.7 ms at 0.33, 6.6 at 0.85)
    double noise;       // > 0: G's frame-to-frame noise amplitude (default 0.04 = +-2%)
    double cpu_until;   // > 0: the CPU time above ends then (s)
    double ease;        // > 0: paced: the share of the way the clock moves each frame (default 0.05; the RP5's settles within
                        // a decision: a step down saves nothing measurable)
    double fill;        // > 0: paced: the clock eases to where G + W is this share of a frame (default 0.9)
    double gpu_read;    // > 0: the reported GPU times are this many times the frame's real GPU wall time (Turnip over-reads)
    double pace_fit_ms; // > 0: the pacer releases when G + W < this for 60 frames (default 13; 10 = backing off; -1 never)
} Case;

typedef struct {
    double f_end, fps_end, hold_end;  // hold_end: the frame rate the controller says it holds (0: none)
    int changes_last60, hold_changes;  // hold_changes: the held rate changed (30 <-> 20 <-> 60), the whole run
    int changes_settled;  // factor changes after the first 20 s
    double back30_t;      // (heavy_until) when it held 30 again after the heavy stretch (0: never)
    double f_min_last60, f_max_last60;
    double hold_t;  // when the held rate last changed (s)
    // (resize_at) after the window's size changed: the factor 1.5 s later, its changes in the next 8 s, frames slower than
    // the frame rate before the change in the next 5 s, steps down and holds ended in the next 15 s (a swing)
    double f_after, f_before;  // (resize_at) the factor 1.5 s after the change / just before it
    int changes_after, slow_after, downs_after;
    int low_win, n_win;
    double f_heavy_min, f_after_heavy;  // (heavy_until) the lowest factor in the heavy stretch / the factor 15 s after it  // 2-s windows under 57 fps / all (a sawtooth hides in "60 fps over the last 10 s")
} Result;

static int near30(double h) { return fabs(h - 30) < 0.5; }
static Result run(const Case *c) {
    g_rng = 7;
    g_t30 = c->t30;
    g_settings.dyn_min = c->min_lines ? c->min_lines : 540;
    double now = 1.0, f = output_scale_dynamic_factor(), g_prev = -1, w_prev = -1;
    double frames_end = 0, t_end = 0, last_f = f, f_min = 1, f_max = 0, last_hold = 0, hold_t = 0;
    int changes = 0, hold_changes = 0, settled = 0;
    double back30 = 0;
    double walk = 0;  // (wander: the load's random part, log scale)
    double clk = 1;   // (clock_slow: the GPU's times x this - 1 at full clock)
    double f_after = -1, f_before = -1, p_before = 0;
    int changes_after = 0, slow_after = 0, downs_after = 0;
    double win_t0 = 1.0; int win_n = 0, low_win = 0, n_win = 0;
    int pace_held = 0, pace_ring[60] = {0}, pace_i = 0, pace_over = 0, pace_fit = 0, pace_holds = 0;
    double pace_grace = 0;
    g_pace_try = 0;
    double f_heavy_min = 2, f_after_heavy = -1;
    printf("%s\n  t=0s %.2f", c->name, f);
    int col = 0;
    while (now < c->secs) {
        OutScale o;
        g_menu = c->wmenu_at > 0 && now > c->wmenu_at && now < c->wmenu_until;
        double lines0 = c->lines > 0 ? c->lines : 2160;
        bool resized = c->resize_at > 0 && now > c->resize_at;
        double lines = resized ? c->lines2 : lines0, area = (lines / lines0) * (lines / lines0);
        output_scale(lines * 16 / 9, lines, false, now, g_prev, w_prev, &o);
        f = output_scale_dynamic_factor();
        if (resized && f_after < 0 && now > c->resize_at + 1.5) f_after = f;
        if (c->resize_at > 0 && !resized) f_before = f;
        if (f != last_f) {
            printf(" | %.0fs %.2f", now, f);
            if (++col % 8 == 0) printf("\n ");
            if (now > c->secs - 60) changes++;
            if (now > 20) settled++;
            if (resized && now < c->resize_at + 8) changes_after++;
            if (resized && now > c->resize_at + 0.1 && now < c->resize_at + 15 && f < last_f) downs_after++;  // (not the change at the switch)
            last_f = f;
        }
        if (c->heavy_until > 0 && now > c->heavy_at && now < c->heavy_until) f_heavy_min = fmin(f_heavy_min, f);
        if (c->heavy_until > 0 && f_after_heavy < 0 && now > c->heavy_until + 15) f_after_heavy = f;
        double hold = output_scale_dynamic_hold_fps();
        if (hold != last_hold) {
            printf(" | %.0fs hold %.0f", now, hold);
            if (++col % 8 == 0) printf("\n ");
            if (c->resize_at > 0 && now > c->resize_at && now < c->resize_at + 15 && hold == 0) downs_after++;
            hold_changes++, last_hold = hold, hold_t = now;
            if (c->heavy_until > 0 && now > c->heavy_until && near30(hold) && !back30) back30 = now;
        }
        if (now > c->secs - 60) { f_min = fmin(f_min, f); f_max = fmax(f_max, f); }
        double scene = (1 + (c->wobble > 0 ? c->wobble : 0.03) * sin(now * 2 * M_PI / 20)) * (c->light_at > 0 && now > c->light_at ? 0.6 : 1.0);
        if (c->heavy_at > 0 && now > c->heavy_at && (c->heavy_until <= 0 || now < c->heavy_until)) scene *= c->heavy;
        if (c->wander > 0) {  // mean-reverting over ~3 s, typical size c->wander
            double gauss = (rnd() + rnd() + rnd() + rnd() - 2) * 1.732, dt = 1 / 60.0;
            walk += -walk / 3.0 * dt + c->wander * sqrt(2 / 3.0) * sqrt(dt) * gauss;
            scene *= exp(walk);
        }
        double w_base = c->w1 > 0 && f >= 1.0 - 1e-6 ? c->w1 : c->w;
        if (c->wf > 0) w_base *= 1 + c->wf * (f - 0.5);
        double nz = c->noise > 0 ? c->noise : 0.04;
        double g = (c->a + c->b * f * f * area) * scene * (1 - nz / 2 + nz * rnd()) * clk, w = w_base * area * (0.99 + 0.02 * rnd()) * clk;
        if (c->paced > 0) {  // the clock eases (over ~0.3 s) to where G + W takes 90% of a 60 Hz frame, never above full
            double full = (g + w) / clk, want = fmin(c->paced, fmax(1.0, (c->fill > 0 ? c->fill : 0.9) * 16.667 / full));
            clk += (want - clk) * (c->ease > 0 ? c->ease : 0.05);
        }
        if (c->wmenu_at > 0 && now > c->wmenu_at && now < c->wmenu_until) w = c->wmenu * (0.99 + 0.02 * rnd());
        double gw = g + w, p = c->fifo ? ceil(gw / 16.667 - 1e-9) * 16.667 : gw;
        if (c->clock_slow > 0) {  // under 60% busy: the clock drifts down (times up), over 80%: back up, over ~1 s
            double busy = gw / fmax(p, 16.667), target = busy < 0.6 ? c->clock_slow : busy > 0.8 ? 1.0 : clk;
            clk += (target - clk) * fmin(1.0, p / 1000.0);
        }
        if (c->cpu > 0 && (c->cpu_until <= 0 || now < c->cpu_until)) p = c->fifo ? ceil((c->cpu + gw) / 16.667 - 1e-9) * 16.667 : c->cpu + gw;
        if (c->t30 && p < 33.333) p = 33.333;
        if (c->pacer) {
            if (pace_held && g_pace_try) {
                pace_held = 0, pace_grace = now + 1, pace_over = pace_fit = 0, memset(pace_ring, 0, sizeof pace_ring);
                printf(" | %.0fs pacer: 60", now);
                if (++col % 8 == 0) printf("\n ");
            }
            g_pace_try = 0;
            if (pace_held) {
                if (p < 33.333) p = 33.333;
                double fit_ms = c->pace_fit_ms > 0 ? c->pace_fit_ms : c->pace_fit_ms < 0 ? -1 : 13;
                pace_fit = fit_ms > 0 && gw < fit_ms ? pace_fit + 1 : 0;
                if (pace_fit >= 60) {
                    pace_held = 0, pace_grace = now + 1, pace_over = 0, memset(pace_ring, 0, sizeof pace_ring);
                    printf(" | %.0fs pacer: 60 (gpu)", now);
                    if (++col % 8 == 0) printf("\n ");
                }
            } else if (now >= pace_grace) {
                int o = p > 17.5;
                pace_over += o - pace_ring[pace_i], pace_ring[pace_i] = o, pace_i = (pace_i + 1) % 60;
                if (pace_over >= 12) {
                    pace_held = 1, pace_fit = 0, pace_holds++;
                    printf(" | %.0fs pacer: 30", now);
                    if (++col % 8 == 0) printf("\n ");
                }
            }
        }
        if (c->stall_at > 0 && now > c->stall_at && now < c->stall_until) p += c->stall_ms;
        if (p < 16.667) p = 16.667;
        if (c->jitter > 0) {
            static double owe;
            static int jn;
            if (owe > 0) p = fmax(1.0, p - owe), owe = 0;
            else if (++jn % 60 == 0) p += c->jitter, owe = c->jitter;
            if (c->miss_every > 0 && jn % c->miss_every == 7) p += 16.667;
        }
        if (c->resize_at > 0 && now > c->resize_at - 3 && now <= c->resize_at) p_before = fmax(p_before, p);
        if (resized && now < c->resize_at + 5 && p > p_before + 1) slow_after++;
        double rd = c->gpu_read > 0 ? c->gpu_read : 1.0;
        g_prev = g * rd, w_prev = w * rd;
        g_pace_slow = c->pacer && pace_held;
        now += p / 1000;
        win_n++; if (now - win_t0 >= 2.0) { n_win++; if (win_n / (now - win_t0) < 57) low_win++; win_t0 = now, win_n = 0; }
        if (now > c->secs - 10) frames_end++, t_end += p / 1000;
    }
    // (by name: the fields' order once differed from this list, so f_min_last60 got hold_t, changes_settled f_min, ...)
    Result r = {.f_end = f, .fps_end = frames_end / t_end, .hold_end = last_hold, .changes_last60 = changes,
                .hold_changes = hold_changes, .changes_settled = settled, .back30_t = back30, .f_min_last60 = f_min,
                .f_max_last60 = f_max, .hold_t = hold_t, .f_after = f_after, .f_before = f_before,
                .changes_after = changes_after, .slow_after = slow_after, .downs_after = downs_after,
                .low_win = low_win, .n_win = n_win, .f_heavy_min = f_heavy_min, .f_after_heavy = f_after_heavy};
    printf("\n  end: factor %.2f, %.1f fps over the last 10 s (holding %.0f), %d change%s in the last 60 s (%.2f-%.2f), %d after 20 s; %d of %d 2-s windows under 57 fps\n",
           r.f_end, r.fps_end, r.hold_end, changes, changes == 1 ? "" : "s", f_min, f_max, settled, low_win, n_win);
    if (c->resize_at > 0)
        printf("  window %.0f -> %.0f lines at %.0f s: factor %.2f 1.5 s later, %d change%s in 8 s, %d slower frame%s in 5 s, "
               "%d step%s down / hold%s ended in 15 s\n", c->lines > 0 ? c->lines : 2160, c->lines2, c->resize_at, f_after,
               changes_after, changes_after == 1 ? "" : "s", slow_after, slow_after == 1 ? "" : "s", downs_after,
               downs_after == 1 ? "" : "s", downs_after == 1 ? "" : "s");
    printf("\n");
    return r;
}

static int g_fail;
static int near(double a, double b) { return fabs(a - b) < 0.5; }
static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n\n", what); g_fail = 1; }
}

int main(int argc, char **argv) {
    // (each case is its own process: the controller's state is static) argv[1]: the case's index
    static const Case cases[] = {
        // 4K window, bilinear: G 16.3 W 0.9 at 0.50 -> 30.6 fps (fifo halving)
        {"4K bilinear (G 16.3 + W 0.9 at 0.50, fifo)", 4.1, 48.8, 0.9, 1, 0, 180},
        // 4K, SMAA + FSR 1: G 13.1 W 20.2 at 0.50 -> 20 fps
        {"4K SMAA + FSR 1 (G 13.1 + W 20.2 at 0.50, fifo)", 3.3, 39.2, 20.2, 1, 0, 180},
        // 1080p, bilinear: G 10.9 W 0.2 at 0.65, hunting 0.65 <-> 0.75 before (the old raise needed G < 10.5, a step
        // down G + W > 14.5): G = 0.3 + 25.1 f^2 (10.9 at 0.65, 14.4 at 0.75), the load swinging +-5%
        {"1080p bilinear (G 10.9 + W 0.2 at 0.65, fifo)", 0.3, 25.1, 0.2, 1, 0, 180, 0.05, 0, 0, 0, 0, 1080},
        // 1080p, SMAA + FSR 1: G 9.1 W 8.1 at 0.50 -> 30 fps (G = 6.5 + 10.4 f^2: a large part that doesn't scale)
        {"1080p SMAA + FSR 1 (G 9.1 + W 8.1 at 0.50, fifo)", 6.5, 10.4, 8.1, 1, 0, 180, 0, 0, 0, 0, 0, 1080},
        // control: the frames don't wait (mailbox): every step down is a faster frame, so down to the floor as before
        {"4K bilinear, mailbox (G 16.3 + W 0.9 at 0.50)", 4.1, 48.8, 0.9, 0, 0, 120},
        // the scene gets lighter (x0.6) at 90 s: 60 fps fits at the floor again
        {"4K bilinear, lighter scene from 90 s (fifo)", 4.1, 48.8, 0.9, 1, 90, 240},
        // pure f^2 GPU (nothing fixed): the most optimistic model for steps down
        {"4K bilinear, G all f^2 (G 16.3 at 0.50, fifo)", 0, 65.2, 0.9, 1, 0, 180},
        // a fast GPU: 4K at 1.0 fits (G 11 + W 1)
        {"fast GPU, 4K (G 11 + W 1 at 1.00)", 2, 9, 1, 1, 0, 60},
        // a busier scene: its load swings +-15%: it follows the scene, the hold stays
        {"4K bilinear, load swinging +-15% (fifo)", 4.1, 48.8, 0.9, 1, 0, 180, 0.15},
        // 1080p bilinear with the load swinging +-15%: steps with the scene, slowly (the raise wait doubles)
        {"1080p bilinear, load swinging +-15% (fifo)", 0.3, 25.1, 0.2, 1, 0, 180, 0.15, 0, 0, 0, 0, 1080},
        // the game's own scaler off (it rendered 1280x720 here before): 1080p at 0.50 G 13.9-14.5 ms, 43.7 fps
        {"1080p bilinear, game scaler off (G 14.2 + W 0.2 at 0.50, fifo)", 3, 44.8, 0.2, 1, 0, 180, 0, 0, 0, 0, 0, 1080},
        // 4K, SMAA + FSR 1 with the game scaler off: G + W 44 ms at 0.50, 30.7 at the floor (0.25): 30 there
        {"4K SMAA + FSR 1, game scaler off (G 23.8 + W 20.2 at 0.50, fifo)", 6, 71.2, 20.2, 1, 0, 180},
        // a hold whose G grows (UHD 770 4K bilinear: 19.4 -> 27.9 ms at 0.50 after holding 30; G mostly fixed here):
        // x1.44 from 60 s - the floor still fits 30
        {"4K bilinear, G x1.44 from 60 s (G 19.4 at 0.50, fifo)", 10, 37.6, 0.9, 1, 0, 180, 0, 60, 1.44},
        // a slower GPU holding 30 at the floor (G 18.2 there), G x1.8 from 60 s: not even the floor fits 30 any more -
        // it stays at the floor, as close to 30 as it gets (the hold stepped down to 20 and climbed there before)
        {"4K bilinear, held 30, G x1.8 from 60 s (fifo)", 14, 37.6, 0.9, 1, 0, 180, 0, 60, 1.8},
        // the same, x1.8 from 60 s to 150 s: the floor, then climbing again once the scene is lighter
        {"4K bilinear, held 30, G x1.8 from 60 s to 150 s (fifo)", 14, 37.6, 0.9, 1, 0, 300, 0, 60, 1.8, 150},
        // x2 swinging +-15% (G 32.7 at the floor, 0.25): at the floor, no hunting
        {"4K bilinear, held 30, G x2 from 30 s, load swinging +-15% (fifo)", 14, 37.6, 0.9, 1, 0, 240, 0.15, 30, 2.0},
        // walking around, a fast GPU at the edge (5120x1440 class: G 13.6 + W 1 at 1.00, the load wandering +-10%):
        // it hunted here in big steps - 45 in 6 min with frames missing. Now it follows the load in 0.05 steps (a step
        // isn't seen, a missed frame is): 60 throughout, a step every ~6 s at most
        {"edge at 1.00, load wandering +-10% (fifo)", 1.6, 12, 1.0, 1, 0, 360, 0.01, 0, 0, 0, 0.10},
        // the same edge lower down (G 13.2 at 0.80), wandering +-15%
        {"edge at 0.80, load wandering +-15% (fifo)", 1.5, 18.3, 0.8, 1, 0, 360, 0.01, 0, 0, 0, 0.15},
        // UHD 770, 1080p window (floor 0.50: G 10.5 there at full clock), a heavy stretch (x1.6, 100-104 s) pushes it into
        // the 30 fps hold; held, the GPU is half idle and clocks down (times x1.5: G ~15.9, as measured on Windows), so
        // the hold's own measure never shows 60 fitting again (it stayed at 30 for the rest of the run). It must probe 60
        {"1080p iGPU, clock drops while held at 30 (fifo)", 2, 34, 0.3, 1, 0, 300, 0, 100, 1.6, 104, 0, 1080, 1.5},
        // fullscreen <-> windowed (issue #205): the window's size changes at 90 s. Held at 30 in a 1080p screen (SMAA +
        // FSR 1, case 3 with a larger fixed part: 60 out of reach at the floor, G 9.7 + W 8.1 at 0.25) to a 1600x900
        // window: the same render height, the hold goes on - no trip to the floor and back
        {"1080p SMAA + FSR 1 held at 30, to a 900-line window at 90 s (fifo)", 9, 10.4, 8.1, 1, 0, 180, 0, 0, 0, 0, 0, 1080,
         0, 90, 900},
        // the same with a heavier window step (W 12 at 1080p): 60 is out of reach in the window too - the hold goes on
        {"1080p heavier window step held at 30, to a 900-line window at 90 s (fifo)", 9, 10.4, 12, 1, 0, 180, 0, 0, 0, 0, 0,
         1080, 0, 90, 900},
        // a 1080p window at 60 (settled at 0.70) made fullscreen on a 4K display: the factor takes about the same render
        // height at once (0.35, on the grid 0.40), the frames stay at 60 (before: 0.7 at 4K, 4x the pixels, until the slow
        // frames stepped it down)
        {"1080p window at 60 to 4K fullscreen at 90 s (fifo)", 0.3, 19, 0.2, 1, 0, 180, 0.05, 0, 0, 0, 0, 1080, 0, 90, 2160},
        // 4K fullscreen at the floor (case 0) to a 1440-line window: the factor goes up to the same render height at
        // once (before: it stayed at 0.33 there for the raise wait, 720 lines of a 1440 window)
        {"4K at the floor to a 1440-line window at 90 s (fifo)", 4.1, 48.8, 0.9, 1, 0, 180, 0, 0, 0, 0, 0, 2160, 0, 90, 1440},
        // Minimum resolution 360p: case 3 in a 1080p window, the floor 0.33 (640x360)
        {"1080p SMAA + FSR 1, Minimum resolution 360p (fifo)", 6.5, 10.4, 8.1, 1, 0, 180, 0, 0, 0, 0, 0, 1080, 0, 0, 0, 360},
        // a heavy 4K scene (G 4 + 200 f^2): 30 at 540p's floor (0.25, G 16.5); 360p's (0.17, G 9.6) fits 60
        {"4K heavy, Minimum resolution 540p (fifo)", 4, 200, 0.9, 1, 0, 180, 0, 0, 0, 0, 0, 2160},
        {"4K heavy, Minimum resolution 360p (fifo)", 4, 200, 0.9, 1, 0, 180, 0, 0, 0, 0, 0, 2160, 0, 0, 0, 360},
        // Minimum resolution 720p: case 2 (settles at 0.70 in a 1080p window), the floor 0.67
        {"1080p bilinear, Minimum resolution 720p (fifo)", 0.3, 25.1, 0.2, 1, 0, 180, 0.05, 0, 0, 0, 0, 1080, 0, 0, 0, 720},
        // Internal FPS target 30, Retroid Pocket 5 (Turnip R6, Field): G + W 15.6 ms at 1.00 in a 1080p window - 1.00
        // holds 30. A warp at 30 s: 3 s of frames 2.5 ms late (shaders compiling, the GPU idle). It went to 0.60 for
        // minutes before (one late half second capped G + W at 15 ms, easing 5% every 30 s); back at 1.00 soon now
        {.name = "target 30, 1.00 fits, 3 s of late frames after a warp at 30 s (fifo)", .a = 1, .b = 13.8, .w = 0.8,
         .fifo = 1, .secs = 150, .lines = 1080, .min_lines = 360, .t30 = 1, .stall_at = 30, .stall_until = 33, .stall_ms = 2.5},
        // Internal FPS target 30 where the CPU's part doesn't overlap the GPU's (Retroid, Mabe, runtime -O0): 20 ms of CPU
        // + G + W 19.8 at 1.00 = frames every 50 ms. It must step down until cpu + G + W fits 33.3 ms, and stay there
        {.name = "target 30, CPU 20 ms not overlapping (G + W 19.8 at 1.00, fifo)", .a = 1, .b = 18, .w = 0.8, .fifo = 1,
         .secs = 180, .lines = 1080, .min_lines = 360, .t30 = 1, .cpu = 20},
        // Internal FPS target 30 at 1.00 (Retroid: G 19 + W 1 ms in the field), the menu open 40-50 s (W 6 ms): the hold
        // must go on. Before, W's change ended it and that half second was judged by 60's budget: 1.00 -> 0.70
        {.name = "target 30 at 1.00, the menu open 40-50 s (W 1 -> 6 -> 1 ms, fifo)", .a = 1, .b = 18, .w = 1, .fifo = 1,
         .secs = 120, .lines = 1080, .min_lines = 360, .t30 = 1, .wmenu_at = 40, .wmenu_until = 50, .wmenu = 6},
        // Internal FPS target 30, a heavier stretch 30-110 s (Retroid, R6: G 24.5 at 1.00, frames every 38-40 ms with 12
        // ms of CPU not overlapping; the upscaler's window step 2.8 ms below 1.00): one step down (0.90 fits) - it went
        // 1.00 -> 0.80 -> 0.70 before (a big first step, then a second for the window step's growth) - and back to 1.00
        {.name = "target 30, a heavier stretch 30-110 s, 1.00 a little late (mailbox)", .a = 1, .b = 14, .w = 0.8,
         .secs = 170, .lines = 1080, .min_lines = 360, .t30 = 1, .cpu = 12, .heavy_at = 30, .heavy = 1.6, .heavy_until = 110},
        // Internal FPS target 30 on a slow GPU (Retroid Pocket 5 in standard performance mode: G 17 + W 5 ms at 0.33, 14 ms
        // of CPU not overlapping - frames every ~36 ms even at the floor): down to the floor, and it stays there - the best it
        // can do for 30. It switched to holding 20 at the
        // floor (the late-frame cap under the floor's G) and then climbed to 1.00 at 20 fps
        {.name = "target 30, slow GPU: the floor a little late (mailbox)", .a = 6, .b = 100, .w = 5, .secs = 150,
         .lines = 1080, .min_lines = 360, .t30 = 1, .cpu = 14},
        // iPad Pro 11-inch M4 (Metal, 2420x1668, MetalFX + SMAA, target 60): the GPU clocks down until it is ~90% busy, so
        // G + W read 14.6-15.0 ms at every factor (game 13.4 + window 1.6 at 1.00 ... 8.8 + 6.0 at 0.32) with the frames on
        // time throughout; MetalFX only runs below 1.00 (window 1.6 -> 3.2 ms). Each step down was predicted to help, none
        // did: 1.00 -> 0.32 (the 540p floor) in 3 s. 60 fits at 1.00 - it must stay there
        {.name = "iPad M4, clock follows the load, 60 fits at 1.00 (fifo)", .a = 2.5, .b = 4.5, .w = 1.6, .w1 = 0.8, .fifo = 1,
         .secs = 120, .lines = 1668, .paced = 2.5},
        // the same GPU, a heavy stretch 40-80 s (G x3: 1.00 misses 60 even at full clock): down while it's heavy, back to
        // 1.00 after it
        {.name = "iPad M4, clock follows the load, heavy 40-80 s (fifo)", .a = 2.5, .b = 4.5, .w = 1.6, .w1 = 0.8, .fifo = 1,
         .secs = 160, .lines = 1668, .paced = 2.5, .heavy_at = 40, .heavy = 3, .heavy_until = 80},
        // the same iPad, the menu open 40-60 s: its window step 0.9 -> 5 ms, G + W 17.5 ms, frames a millisecond late.
        // Dynamic stepped 1.00 -> 0.55 behind the menu; it must stay at 1.00, with the menu open and after it closes
        {.name = "iPad M4, clock follows the load, the menu open 40-60 s (W 5 ms, fifo)", .a = 2.5, .b = 4.5, .w = 1.6,
         .w1 = 0.8, .fifo = 1, .secs = 120, .lines = 1668, .paced = 2.5, .wmenu_at = 40, .wmenu_until = 60, .wmenu = 5},
        // Retroid Pocket 5 at target 60 (Turnip R6, 1080p, FSR 1 + SMAA: W 3.9 ms): the Adreno's clock follows the load like
        // the iPad's (G + W 14.3-15.6 ms at 0.50 ... 0.33, steps saving nothing), and present times jitter (a 20-23 ms frame
        // every second or two, no vsync missed). Paced, it raised only after a second of frames averaging under 16.9 ms:
        // the jittered frames broke that every time and it sat at 0.45 (486p) for minutes with 60 fps throughout
        {.name = "RP5, clock follows the load, frame times jitter (fifo)", .a = 3, .b = 9, .w = 3.9, .fifo = 1, .secs = 150,
         .lines = 1080, .paced = 2.0, .jitter = 5, .miss_every = 150},
        // Retroid Pocket 5 (Turnip R6, 1080p, FSR 1 + SMAA: W 3.3 ms; G 3.8 at 0.50 in a light scene), target 60, with the
        // frame pacer: a heavy stretch 30-60 s (x3.5: the floor misses 60) holds the game at 30. Held, the Adreno clocks
        // down (times x1.6) and the hold climbs, so the pacer's own way back (the GPU under 13 ms) never comes; Dynamic's
        // probes for 60 dropped to the floor, but the game stayed in its 30 fps mode - so each failed and backed off. It
        // stayed at 30 with 60 fitting at 0.80. The probe must hand 60 to the pacer: back to 60 after the heavy stretch
        {.name = "RP5, pacer holds 30, clock drops, heavy 30-60 s (fifo)", .a = 4.5, .b = 10, .w = 3.3, .fifo = 1, .secs = 240,
         .lines = 1080, .clock_slow = 1.6, .heavy_at = 30, .heavy = 2.0, .heavy_until = 60, .pacer = 1},
        // the RP5 / pacer review cases (Retroid Pocket 5 logs, 2026-11-12): late-frame runs, W growing with the factor,
        // GPU times over-reading the frame, the pacer backing off
        // paced GPU where one grid step (0.90) has full-clock G + W = 16.4 ms: ~8% late frames (noise 4%)
        {.name = "paced, 0.90 is 8% late (noise 4%)", .a = 3, .b = 11.72, .w = 3.9, .fifo = 1, .secs = 240,
         .lines = 1080, .paced = 2.0, .jitter = 5, .miss_every = 150},
        // the same with 10% G noise: a wider partial-late band
        {.name = "paced, 0.90 is partially late (noise 10%)", .a = 3, .b = 11.72, .w = 3.9, .fifo = 1, .secs = 240,
         .lines = 1080, .paced = 2.0, .jitter = 5, .miss_every = 150, .noise = 0.10},
        // RP5 at 30 with the pacer: W grows with the factor (4.7 at 0.33 -> 6.6 at 0.85 in the 215406 log)
        {.name = "held 30, W grows with the factor (RP5 FSR 1)", .a = 4.5, .b = 10, .w = 4.7, .wf = 0.8, .fifo = 1, .secs = 240,
         .lines = 1080, .min_lines = 360, .clock_slow = 1.6, .heavy_at = 30, .heavy = 2.0, .pacer = 1},
        // 1080p SMAA + FSR 1 real hold (case 3) with W growing 30% over the climb
        {.name = "1080p SMAA + FSR 1 held 30, W grows with the factor", 6.5, 10.4, 8.1, 1, 0, 300, 0, 0, 0, 0, 0, 1080,
         .wf = 0.86},
        // pacer backing off (release needs G + W < 10): a light floor whose clocked-down G + W at 30 is under 14.5
        {.name = "pacer backing off, light floor (G+W 10.4 at the floor at 30)", .a = 3, .b = 6, .w = 2, .fifo = 1, .secs = 240,
         .lines = 1080, .clock_slow = 1.6, .heavy_at = 20, .heavy = 3.0, .heavy_until = 40, .pacer = 1, .pace_fit_ms = 10},
        // the same, pacer first try (13 ms)
        {.name = "pacer first try, light floor", .a = 3, .b = 6, .w = 2, .fifo = 1, .secs = 240,
         .lines = 1080, .clock_slow = 1.6, .heavy_at = 20, .heavy = 3.0, .heavy_until = 40, .pacer = 1},
        // CPU-bound pacer hold at target 60 (fast GPU: G 11 + W 1 at 1.00): the pacer never releases; Dynamic?
        {.name = "CPU-bound pacer hold, GPU 12 ms at 1.00", .a = 2, .b = 9, .w = 1, .fifo = 1, .secs = 120,
         .lines = 1080, .cpu = 20, .pacer = 1, .pace_fit_ms = -1},
        // a real hold for 10 minutes: how often does it probe (each probe: the floor + 60 mode slow motion)
        {.name = "1080p SMAA + FSR 1 real hold, 600 s, pacer", 6.5, 10.4, 8.1, 1, 0, 600, 0, 0, 0, 0, 0, 1080, .pacer = 1},
        // case 35 with the load wandering 5%: how long until it counts as paced
        {.name = "RP5 paced, load wandering 5%", .a = 3, .b = 9, .w = 3.9, .fifo = 1, .secs = 150,
         .lines = 1080, .paced = 2.0, .jitter = 5, .miss_every = 150, .wander = 0.05},
        // case 36 without the clock drop: the pacer may release on its own while Dynamic still holds 30
        {.name = "pacer holds 30, no clock drop, heavy 30-60 s", .a = 4.5, .b = 10, .w = 3.3, .fifo = 1, .secs = 240,
         .lines = 1080, .heavy_at = 30, .heavy = 2.0, .heavy_until = 60, .pacer = 1},
        // a short heavy burst (2 s) holds the pacer, backing off (release under 10 ms); mild clock drop x1.2; the floor's
        // G + W at 30 reads 12 ms: Dynamic neither holds nor climbs, the pacer never sees < 10
        {.name = "pacer backing off after a 2 s burst, mild clock drop, floor reads 12 ms", .a = 5, .b = 8, .w = 3, .fifo = 1,
         .secs = 180, .lines = 1080, .clock_slow = 1.2, .heavy_at = 20, .heavy = 3.0, .heavy_until = 22.5, .pacer = 1, .pace_fit_ms = 10},
        // the same on the pacer's first try (13 ms)
        {.name = "pacer first try after a 2 s burst, mild clock drop, floor reads 12 ms", .a = 5, .b = 8, .w = 3, .fifo = 1,
         .secs = 180, .lines = 1080, .clock_slow = 1.2, .heavy_at = 20, .heavy = 3.0, .heavy_until = 22.5, .pacer = 1},
        // CPU-bound for 30 s then not (GPU 12 at 1.00), pacer backing off
        {.name = "CPU-bound 30 s then light, pacer backing off", .a = 2, .b = 9, .w = 1, .fifo = 1, .secs = 120,
         .lines = 1080, .cpu = 20, .cpu_until = 30, .pacer = 1, .pace_fit_ms = 10},
        // a 1 s hitch burst (every frame +17 ms) holds the pacer, backing off (release under 10 ms); mild clock drop x1.2;
        // 60 fits at 1.00 (G 11 + W 2 = 13 ms at full clock)
        {.name = "hitch burst holds the pacer (backing off), mild clock drop, 60 fits at 1.00", .a = 3, .b = 8, .w = 2, .fifo = 1,
         .secs = 180, .lines = 1080, .clock_slow = 1.2, .stall_at = 20, .stall_until = 21.5, .stall_ms = 17, .pacer = 1, .pace_fit_ms = 10},
        {.name = "the same, pacer first try (13 ms)", .a = 3, .b = 8, .w = 2, .fifo = 1,
         .secs = 180, .lines = 1080, .clock_slow = 1.2, .stall_at = 20, .stall_until = 21.5, .stall_ms = 17, .pacer = 1},
        // the RP5 on 44e00622 (lasrecomp.log t=40-320, target 60, Minimum 360p): G mostly fixed (game 12.2 at 0.60,
        // 13.7 at 0.80 with the clock at its top), W 4.2 at 0.55 -> 5.0 at 0.80, GPU times over-read the frame (gpu 18.3 ms
        // with frames every 18.2 = 1 in 12 late), a mild clock drop, a 35 ms hitch every few seconds
        {.name = "RP5 on 44e00622: W grows, GPU over-reads 1.14x, clock fills to 77%", .a = 8.8, .b = 5.0, .w = 3.6, .wf = 0.6,
         .fifo = 1, .secs = 300, .lines = 1080, .min_lines = 360, .paced = 1.3, .fill = 0.77, .gpu_read = 1.14, .noise = 0.06, .wander = 0.03,
         .jitter = 5, .miss_every = 150},
        // the same, lighter (0.90 is the edge)
        {.name = "RP5 shape, lighter (edge near 0.95)", .a = 7.6, .b = 5.0, .w = 3.6, .wf = 0.6,
         .fifo = 1, .secs = 300, .lines = 1080, .min_lines = 360, .paced = 1.3, .fill = 0.77, .gpu_read = 1.14, .noise = 0.06, .wander = 0.03,
         .jitter = 5, .miss_every = 150},
        // the same, a heavy stretch 100-160 s (x1.15) then light again: must step down in it and come back after
        {.name = "RP5 shape, heavy 100-160 s (x1.12)", .a = 8.8, .b = 5.0, .w = 3.6, .wf = 0.6,
         .fifo = 1, .secs = 300, .lines = 1080, .min_lines = 360, .paced = 1.3, .fill = 0.77, .gpu_read = 1.14, .noise = 0.06, .wander = 0.03,
         .jitter = 5, .miss_every = 150, .heavy_at = 100, .heavy = 1.12, .heavy_until = 160},
        // the case above with a steadier scene (wander 1%): 0.80 is 7% late all the time, as the log's t=160-206 (55 fps for 46 s)
        {.name = "RP5 shape, steady scene: 0.80 is 7% late", .a = 8.8, .b = 5.0, .w = 3.6, .wf = 0.6,
         .fifo = 1, .secs = 300, .lines = 1080, .min_lines = 360, .paced = 1.3, .fill = 0.77, .gpu_read = 1.14, .noise = 0.06, .wander = 0.01,
         .jitter = 5, .miss_every = 150},
        // the RP5 on fb357faf (2026-11-12 23:03 log): the clock fills to G + W ~14.4 ms, right at the budget, so steps
        // down come one at a time, 46 s apart (0.55 -> 0.50 ... 0.45 -> 0.40), each saving nothing - but never two
        // within 5 s, so it never counted as paced, and a raise was never predicted to fit: 432p at 60 fps for minutes
        {.name = "RP5 paced, G + W right at the budget: futile steps far apart", .a = 3, .b = 9, .w = 3.9, .fifo = 1,
         .secs = 300, .lines = 1080, .min_lines = 360, .paced = 2.0, .fill = 0.858, .ease = 0.3, .noise = 0.04, .wander = 0.03, .jitter = 5,
         .miss_every = 150},
        // Android / iOS: paced from the start. The RP5 (2026-11-12 23:19 log) holds 60 at 720p (0.67) and more, but on
        // desktop rules it sank 1.00 -> 0.45 in its first seconds (each step "saving" because the level was still loading)
        // and sat at 486p. The same two shapes, paced from the start
        {.name = "Android RP5 shape, paced from the start", .a = 8.8, .b = 5.0, .w = 3.6, .wf = 0.6,
         .fifo = 1, .secs = 300, .lines = 1080, .min_lines = 360, .paced = 1.3, .fill = 0.77, .gpu_read = 1.14, .noise = 0.06,
         .wander = 0.03, .jitter = 5, .miss_every = 150, .start_paced = 1},
        {.name = "Android RP5, G + W at the budget, paced from the start", .a = 3, .b = 9, .w = 3.9, .fifo = 1,
         .secs = 300, .lines = 1080, .min_lines = 360, .paced = 2.0, .fill = 0.858, .ease = 0.3, .noise = 0.04,
         .wander = 0.03, .jitter = 5, .miss_every = 150, .start_paced = 1},
        // the RP5 through the swamp (2026-11-12 23:24 log): a heavy stretch steps it down to the 360p floor, a ceiling at
        // each step with the wait doubling at each (to a minute), and the floor's step never matched the early lift - 360p
        // for a minute after the swamp. Back up within 15 s of it now
        {.name = "Android RP5, a heavy stretch 60-100 s (the swamp), paced from the start", .a = 8.8, .b = 5.0, .w = 3.6, .wf = 0.6,
         .fifo = 1, .secs = 200, .lines = 1080, .min_lines = 360, .paced = 1.3, .fill = 0.77, .gpu_read = 1.14, .noise = 0.06,
         .jitter = 5, .miss_every = 150, .start_paced = 1, .heavy_at = 60, .heavy = 1.6, .heavy_until = 100},
        // the iPad (case 32) with a 35 ms hitch every 2.5 s: stays at 1.00
        {.name = "iPad paced + hitches every 2.5 s", .a = 2.5, .b = 4.5, .w = 1.6, .w1 = 0.8, .fifo = 1,
         .secs = 120, .lines = 1668, .paced = 2.5, .jitter = 3, .miss_every = 150},
    };
    int n = sizeof cases / sizeof *cases;
    if (argc < 2) { printf("%d\n", n); return 0; }
    int i = atoi(argv[1]);
    if (cases[i].start_paced) setenv("LARECOMP_DYN_PACED", "1", 1);
    Result r = run(&cases[i]);
    // (Minimum resolution 540p unless a case says: 0.50 in a 1080p window, 0.25 at 4K)
    switch (i) {
    case 0: check(r.f_end >= 0.35 && r.fps_end > 58 && r.changes_last60 == 0, "4K bilinear: 60 under 0.50"); break;
    case 1: check(r.fps_end > 28 && r.changes_last60 <= 2, "4K FSR: 30 fps at the floor"); break;
    case 2: check(r.fps_end > 58 && r.changes_last60 == 0 && r.f_end >= 0.65, "1080p bilinear: settle at 60"); break;
    // (a 1080p window's floor is 0.50 at 540p, 960x540: 60 is out of reach there, so it holds 30 - much sharper)
    case 3: check(near(r.hold_end, 30) && r.fps_end > 29 && r.changes_last60 <= 2, "1080p FSR: 30, sharper"); break;
    case 4: check(r.fps_end > 58 && r.f_end <= 0.45, "mailbox: 60 under 0.50"); break;
    case 5: check(r.fps_end > 58, "lighter scene: back to 60"); break;
    case 6: check(r.f_end >= 0.35 && r.fps_end > 58 && r.changes_last60 == 0, "4K bilinear f^2: 60 under 0.50"); break;
    case 7: check(r.f_end == 1.0 && r.fps_end > 58, "fast GPU: full resolution"); break;
    case 8: check(r.fps_end > 58 && r.changes_last60 <= 2, "4K bilinear +-15%: 60, steady"); break;
    case 9: check(r.fps_end > 58 && r.changes_last60 <= 10, "1080p bilinear +-15%: 60, following the scene in small steps"); break;
    case 10: check(r.fps_end > 58 && r.changes_last60 == 0, "1080p game scaler off: 60"); break;
    case 11: check(r.fps_end > 29 && r.f_end < 0.26 && r.changes_last60 <= 2, "4K FSR 44 ms: 30 at the floor"); break;
    case 12: check(near(r.hold_end, 30) && r.fps_end > 28 && r.changes_last60 <= 2, "G x1.44: hold 30 at a lower factor"); break;
    case 13: check(near(r.hold_end, 30) && r.f_end < 0.26 && r.fps_end > 19 && r.changes_last60 <= 2 && r.hold_changes <= 5,
                   "G x1.8: at the floor, as close to 30 as it gets - no 20 hold"); break;
    // (or a look for 60 from the hold just began at the end)
    case 14: check((near(r.hold_end, 30) || r.hold_t > 297) && r.fps_end > 28 && r.f_max_last60 >= 0.4,
                   "G x1.8 then back: the floor, then climbing again"); break;
    case 15: check(near(r.hold_end, 30) && r.f_end < 0.26 && r.fps_end > 19 && r.changes_last60 <= 2,
                   "G x2 +-15%: at the floor, no hunting - no 20 hold"); break;
    case 16: check(r.changes_settled <= 60 && r.fps_end > 58, "edge at 1.00 wandering: 60, small steps at most every ~6 s"); break;
    case 17: check(r.changes_settled <= 60 && r.fps_end > 58, "edge at 0.80 wandering: 60, small steps at most every ~6 s"); break;
    case 18: check(r.fps_end > 58 && r.hold_end == 0 && r.hold_t < 150, "iGPU clock lock-in: back to 60 after the heavy stretch"); break;
    // (held 30 in a smaller window: the hold goes on - or 60 fits at the window's floor now, found by a probe)
    case 19: check(r.f_after >= r.f_before - 1e-6 && r.slow_after == 0 && (near(r.hold_end, 30) ? r.changes_after <= 1 : r.fps_end > 58),
                   "held 30, to a smaller window: no swing"); break;
    case 20: check(r.downs_after == 0 && r.f_after >= r.f_before - 1e-6 && r.slow_after == 0 && near(r.hold_end, 30),
                   "held 30 with 60 out of reach, to a smaller window: the hold goes on"); break;
    case 21: check(r.f_after <= 0.4 && r.slow_after <= 3 && r.fps_end > 58 && r.downs_after == 0,
                   "1080p window to 4K: the same render height, still 60"); break;
    case 22: check(r.f_after >= 0.5 && r.fps_end > 58, "4K floor to a 1440 window: the same render height at once"); break;
    case 23: check(r.f_min_last60 >= 0.33 && r.fps_end > 29 && r.changes_last60 <= 2, "360p: floor 0.33, steady"); break;
    case 24: check(near(r.hold_end, 30) && r.fps_end > 29 && r.f_min_last60 >= 0.25 && r.changes_last60 <= 2, "4K heavy 540p: 30 at 0.25"); break;
    case 25: check(r.fps_end > 58 && r.f_end < 0.25 && r.changes_last60 <= 2, "4K heavy 360p: 60 under 0.25"); break;
    case 26: check(r.fps_end > 58 && r.f_min_last60 >= 0.66 && r.changes_last60 <= 2, "720p: never under 0.67"); break;
    case 27: check(r.f_end == 1.0 && r.fps_end > 29 && r.f_min_last60 >= 0.9, "target 30, 1.00 fits: back at 1.00 after the warp"); break;
    case 28: check(r.fps_end > 29 && r.f_end < 0.8 && r.changes_last60 <= 2, "target 30, CPU not overlapping: steps down to 30, steady"); break;
    case 29: check(r.f_end == 1.0 && r.changes_settled == 0 && r.fps_end > 29, "target 30, menu open and closed: stays at 1.00"); break;
    case 30: check(r.f_end == 1.0 && r.fps_end > 29 && r.f_min_last60 >= 0.89, "target 30, heavier stretch: one step down, then back"); break;
    case 32: check(r.f_end == 1.0 && r.f_min_last60 == 1.0 && r.fps_end > 58 && r.changes_settled <= 2,
                   "iPad M4, clock follows the load: stays at 1.00"); break;
    case 34: check(r.f_end == 1.0 && r.changes_settled == 0 && r.fps_end > 58, "iPad M4, menu open and closed: stays at 1.00"); break;
    case 33: check(r.f_end == 1.0 && r.fps_end > 58, "iPad M4, heavy stretch: back to 1.00 after it"); break;
    case 35: check(r.f_end >= 0.95 && r.fps_end > 58, "RP5, clock follows the load, jitter: back up to full size"); break;
    case 36: check(r.fps_end > 58 && r.f_end >= 0.7, "RP5, pacer held at 30: back to 60 after the heavy stretch"); break;
    case 37: case 38: check(r.fps_end > 58 && r.low_win * 10 <= r.n_win, "paced, one step partly late: under it, few slow windows"); break;
    case 40: check(r.changes_settled <= 2 && near(r.hold_end, 30), "held 30, W grows with the factor: the hold goes on, no cycling"); break;
    case 43: check(r.f_end == 1.0, "CPU-bound pacer hold, GPU fits 60: no step down for nothing"); break;
    case 44: check(r.changes_settled <= 2, "a hold 60 can't reach: no probes every 2 minutes"); break;
    case 52: case 53: case 54: check(r.low_win * 10 <= r.n_win, "RP5 shape: at most 1 in 10 windows under 57 fps"); break;
    case 55: check(r.low_win <= 4 && r.changes_settled <= 20 && r.f_end >= 0.7, "RP5, steady scene: settles under the late step, no sawtooth"); break;
    case 56: check(r.f_end >= 0.95 && r.f_min_last60 >= 0.9, "RP5 paced at the budget: found paced, not stuck low"); break;
    case 57: check(r.low_win * 10 <= r.n_win && r.f_min_last60 >= 0.6, "Android RP5 shape, paced from the start: never near 486p, few slow windows"); break;
    case 58: check(r.f_end >= 0.95 && r.f_min_last60 >= 0.9, "Android RP5 at the budget, paced from the start: high"); break;
    case 59: check(r.f_heavy_min < 0.5 && r.f_after_heavy >= 0.6, "Android RP5 through the swamp: back up within 15 s"); break;
    case 60: check(r.f_end == 1.0 && r.low_win == 0, "iPad paced with hitches: stays at 1.00"); break;
    case 31: check(near(r.hold_end, 30) && r.f_max_last60 <= 0.45 && r.hold_changes <= 2 && r.fps_end > 26,
                   "target 30, slow GPU: at the floor, held at 30 - never 20 and climbing"); break;
    }
    // a hold is real: the frames come at least at the rate it says (target 30 at its floor: as close as it gets)
    if (r.hold_end > 0 && i != 31 && i != 13 && i != 15) check(r.fps_end >= 0.95 * r.hold_end, "the held frame rate is met");
    return g_fail;
}
