// Output resolution, shared by the Metal and Vulkan backends (each calls output_scale() once per presented frame).
// Render targets the game sizes for the Switch's 1920x1080 screen are allocated at (sx, sy) times their size; the
// image is then drawn into (vx, vy, vw, vh) of the window. Video tab "Internal resolution" (g_settings.res_mode):
//   Match display  render at the window's own pixel size (default)
//   1x .. 4x       N times the Switch's docked image: 1080 lines x N (wide windows keep their shape: 32:9 at 2x =
//                  7680 x 2160); capped to the GPU's 16384-pixel texture limit. 1x on an integrated GPU: 720 lines,
//                  what the game's own scaler picked there (UHD 770); its scaler is off now (see rt_game_dynres)
//   .75x / .50x / .25x  of Match display (scaled up to the window - with MetalFX on macOS when chosen)
//   Dynamic        Match display, lowered in steps while the GPU can't hold 60 fps and raised again when it can.
//                  The render targets stay allocated at the Match display size (ax, ay) and the frame is drawn into
//                  their top-left part at (sx, sy): no reallocation at a step, so no hitch (each step reallocated
//                  every screen-sized texture before: ~325 ms on an M3 Max, seconds at 4K on Vulkan, issue #100).
// Test overrides: LARECOMP_SCALE=<f> (render height = 1080 x f), LARECOMP_WIDE_TEST=<f> (forced shape),
// LARECOMP_DYN_TEST=<f> (Dynamic pinned at factor f of the allocation: shots at a fixed part), LARECOMP_DYN_START=<f>
// (Dynamic starts at factor f).
#include <math.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "output_scale.h"
#include "pace.h"

#define MAX_TEX 16384.0

// The game's own dynamic resolution (tools/patches.py GAME_DYNRES_SITE) is off: the resolution is the one chosen here,
// and only our Dynamic changes it. LARECOMP_GAME_DYNRES=1 (test): the game lowers its main pass under GPU load again.
int rt_game_dynres(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_GAME_DYNRES") && atoi(getenv("LARECOMP_GAME_DYNRES"));
    return on;
}

// Dynamic resolution: a factor on the Match display scale, from the floor to 1 in coarse 0.1 steps. The floor is the
// Video tab's Minimum resolution (g_settings.dyn_min): 360, 540 (the default) or 720 render lines - 540 is 0.50 of a
// 1080p window and 0.25 at 4K; a display that many lines or fewer: no scaling. It was 540 lines but never under 1/3
// of the display (issue #205).
// It holds a resolution and changes only when it must: down when 3 of the last 4 half seconds were over budget or
// they averaged over 95% of it (1 s in a row when frames miss 60 outright), straight to where the last second's frame is predicted to fit with room; up
// one step after 5 s in a row (no load spikes in it) with that step predicted to fit with room, at least 5 s after the
// last change, and the wait before a raise doubles each time a raise is undone within 30 s (up to a minute; reset
// after a calm minute). Before: down after one half second over, up after 3 s in 0.05 steps - it stepped all the time
// (45 changes in 6 min at 5120x1440 before the GPU time fix #197; 25 in 5 min at 7680x4320 on an M3 Max after it).
// It reacts to the GPU only: a lower resolution doesn't help a frame the CPU / the game made slow (a level streaming
// in, a shader compiling: 100+ ms frames with the GPU nearly idle, which used to step it down on a fast GPU and, with
// the slow raises, keep it low for a minute). With GPU times (Metal's command buffers, Vulkan's timestamps) it goes by
// those alone. Without, by the frame times with the spikes left out: the mean of the half second's frames up to 3x their
// median (a frame missing one vsync, 33 ms, still counts: that's how a slow GPU shows at 60 Hz). Either way a spike
// neither steps it down nor, so, doubles the raise wait. LARECOMP_DYN_NO_GPU=1: ignore the GPU times (tests the
// frame-time path); LARECOMP_SPIKE_MS (pace.c): CPU-side spikes.
//
// The GPU time has two parts: the game's (G, scales with the factor: about its area, factor^2) and the window step's
// (W: the image drawn into the window - upscale, SMAA - and the menu: depends on the window's size only). The budget
// is for G + W (87% of a 60 Hz frame); a raise must be predicted to fit with room to spare (G x (new / old)^2 with 25%
// on top, + W: the room is for G, which moves with the scene - W is the window's and steady, and large at 4K / 8K
// with an upscaler: 7-9 ms at 7680x4320 with MetalFX on an M3 Max; no fixed raise threshold, so it settles one step
// under where it would go over instead of hunting), and a step down goes straight to where G + W is predicted to fit
// with that same room (at most 0.3 at once), so the next raise isn't predicted to fit right away.
// Held: when even the floor misses 60 fps and the frames wait for the display (vsync: a frame every 33 ms
// for G + W = 17), going lower gains nothing, so the budget becomes 87% of that frame interval (whole 60 Hz refreshes:
// 33.3 / 50 ms) and the factor goes back up as far as it fits there (a large W - 4K SMAA / FSR 1 on an integrated
// GPU - or a slow GPU at a big window). A hold keeps measuring: G + W over the hold's budget for a second lowers the
// factor; at the floor it stays there, as close to 30 as it gets (it stepped down to 20 fps there before: UHD 770 4K
// 19.4 -> 27.9 ms, frames every 49-65 ms - DYN_HOLD_MAX). It steps back up - 30 to looking for
// 60 - only when the floor is predicted to fit the faster rate with 20% to spare for 2 s (after 10 s, doubling each
// time that fails or is undone within 30 s), and starts over when W changes (another upscaler / anti-aliasing / window
// size). Frames that don't wait (mailbox: a frame every G + W) keep going down to the floor: there every step down is
// a faster frame. tools/test_dynres.py: the controller against a simulated GPU (the UHD 770 cases).
#define DYN_DEC_S 0.25      // s per decision, at any frame rate (it was 30 frames: half a second at 60, a whole one at
                            // 30 - every wait twice as long at Internal FPS target 30)
#define DYN_NMAX 64         // frames a decision holds at most
#define DYN_STEP 0.05       // the factor's grid (the floor below its first step): small steps, often - changing the
                            // render scale is free (Dynamic draws into part of the targets), small steps don't show
#define DYN_BUDGET 14.5     // ms of GPU work per frame at 60 fps (G + W)
#define DYN_RAISE_FIT 0.9   // a hold's raise: predicted G + W under this share of the budget
#define DYN_UP_FIT 0.8      // a raise / a step down's target (not held): predicted G / this + W within the budget
#define DYN_DOWN_VOTES 2    // decisions over budget out of the last 4 (1 s) before a step down
#define DYN_DOWN_FAST 2     // ... in a row when the frames miss 60 outright: G + W over a whole refresh (0.5 s)
#define DYN_UP_VOTES 4      // decisions in a row with the next step predicted to fit before a raise (1 s)
#define DYN_RAISE_WAIT 1.0  // s from the last change to a raise (doubles while raises are undone, up to 16)
#define DYN_RAISE_WAIT_MAX 16.0
#define DYN_DOWN_SMALL 1.15 // G + W over the budget by at most this: one step down (more: straight to where it fits)
#define DYN_MAX_DOWN 0.3    // the most a step down lowers the factor at once
#define DYN_HOLD_UP_FIT 0.8 // a hold steps up: the floor's predicted G + W under this share of the faster rate's budget
#define DYN_HOLD_MAX 2      // vsyncs a hold waits at most (60 Hz: 30 fps). It was 3 (20 fps): when even the floor
                            // missed 30, the hold went to 20 and the factor climbed while 20 fit - a sharper image at
                            // 20 fps. The floor as close to 30 as it gets is the better trade (Retroid Pocket 5 in
                            // standard performance mode)
#define DYN_MENU_SETTLE 1.0 // s after the menu closes before Dynamic decides again (its last frames, W settling)
static double g_dyn = 1.0;
static double g_dyn_floor = 0.5;  // (set from the setting and the window: dyn_set_floor)
static unsigned g_down_hist;  // the last decisions over budget (bit 0: the latest)
static double g_down_gw[4];   // and their G + W
static int g_down_fast_votes;  // decisions in a row missing 60 outright
static double g_prev_G = -1, g_prev_W;  // the decision before's G and W (a step down's target: the two averaged)
static double g_dyn_gpu_sum, g_dyn_win_sum, g_last_t, g_dyn_dt[DYN_NMAX], g_dec_t0 = -1;
static int g_dyn_n, g_dyn_gpu_n, g_dyn_good;
static double g_dyn_cool_until, g_dyn_raise_wait = DYN_RAISE_WAIT, g_dyn_raise_at;
static double g_dyn_last_raise = -100, g_dyn_last_lower = -100;
static double g_hold_dt, g_hold_w;               // held: the frame interval at the floor (> 0) and W then
static double g_probe_at, g_probe_wait = 10.0;  // looking for 60 again (from a hold)
static bool g_probing;
static double g_probe_start = -100, g_probe_ok_at = -100;  // when the last probe for 60 began / was last met
static int g_probe_good;                                     // probing: decisions in a row that met 60
static double g_probe_from;                                  // the factor the hold had when the probe began
static double g_probe_fail_gw, g_probe_fail_at = -1000;      // the last failed probe: G + W at the floor then, when
#define DYN_PROBE_MAX 60.0  // s: the longest wait between probes for 60 from a hold
// held: G at the floor followed through the scene's load - measured there as the hold began, then scaled by how G
// changed at each factor since (G right after a step is the reference at the new factor): the part of G that
// doesn't scale with the resolution makes area estimates optimistic (UHD 770 1080p: ~6.5 of 9.1 ms at 0.50)
static double g_floor_g, g_ref_g;
// Internal FPS target 30: G + W just under where the frames missed 30 (0: none yet), and when it last missed or eased
static double g_t30_cap, g_t30_at;
static int g_t30_late;  // (half seconds in a row with the frames late at target 30)
static double g_t30_wait = 4, g_t30_off_at = -1e9;  // (on time this long lifts the cap; when it was last lifted)
static bool g_t30_limits;  // (the cap is what the budget is this half second: a step down is one step)
static bool g_ref_pending;
static int g_hold_miss, g_hold_up_votes;  // held: decisions in a row over the hold's budget / with room for the next rate
static int g_hold_met_votes;              // held at 20: decisions in a row whose frames already met 30 (with G + W in its budget)
static double g_hold_up_at = -100;        // when the hold last stepped up (20 -> 30)

// The GPU's clock follows the load (Apple GPUs; iPad Pro M4 at 60 fps): with room to spare it clocks down until it is
// ~90% busy, so G + W reads ~15 ms at every factor and says nothing about the room left. There a step down made each
// frame no cheaper (game 13.4 + window 1.6 ms at 1.00, 8.8 + 6.0 at 0.32 - MetalFX starts below 1.00 - frames on time
// throughout), and each looked over budget again: 1.00 -> the 540p floor in 3 s. A step down taken with the frames on
// time is checked: the decision after it must show at least a third of its predicted saving in G + W (W counts: MetalFX
// starts below 1.00). Two in a row that don't (the menu opening between a step and its check: one)
// (or the scene getting heavier, or the part of G that doesn't scale) undo that whole run of steps down (back
// to where it began, 1.00 on the iPad) and the GPU counts as paced
// from then on: down only when frames come late (the clock is at its top then, G honest), up when they've been on time
// (a raise that makes them late is undone, the raise wait doubling as usual).
// Phones, handhelds and tablets (Android, iOS / iPadOS) count as paced from the start: their GPUs all clock to the load,
// and finding it out took two futile steps down - a Retroid Pocket 5 (Turnip, G + W read 14.4-14.9 ms at every factor)
// sank 1.00 -> 0.45 at 60 fps in its first seconds and sat there until it happened to step down again, sometimes for
// minutes. LARECOMP_DYN_PACED=1 / 0: paced from the start / found by futile steps (as on desktops).
static bool g_paced;
static double g_chk_from = -1, g_chk_gw, g_chk_pred;  // the last step down: the factor before, G + W then, predicted after
static int g_futile;                                  // steps down in a row that saved too little
static double g_futile_at;                            // when the last was seen
// Paced, frames came late at g_pc_ceil: raises stay under it until g_pc_until (a wait of 10 s, doubling to a minute when
// it's hit again within 30 s of the last lift; 10 s again after 2 quiet minutes). The GPU time can't predict a raise
// here (it reads ~90% of the frame at any factor, more than the frame on a Retroid: 18.3 ms with frames every 18.2), so
// without it a raise came whenever the frames had been on time: 0.55 -> 0.80 a step a second, late, straight back to
// 0.55, every ~10 s - a third of the time at 50-55 fps. One step under it the clock is near its top, so G there is honest:
// G 10% under its first measure there (the scene got lighter) lifts it early (g_pc_ref_f / g_pc_ref_g).
static double g_pc_ceil = 2.0, g_pc_until = -1e9, g_pc_wait = 10.0, g_pc_hit_at = -1e9, g_pc_ref_f = -1, g_pc_ref_g = -1;
static double g_chain_from = -1, g_chain_at;  // the factor before this run of steps down taken with frames on time; the last
#define DYN_CHECK_SHARE 0.33  // a step down's measured saving at least this share of the predicted one

static int hold_vsyncs(void) { return (int)lround(g_hold_dt * 60 / 1000); }
static bool at_floor(void) { return g_dyn <= g_dyn_floor + 1e-6; }
// the grid, the floor below its first step (and no step closer than half a step above the floor)
static double dyn_grid(double f) {
    if (f <= g_dyn_floor + DYN_STEP / 2 - 1e-6) return g_dyn_floor;
    return fmin(1.0, fmax(g_dyn_floor, round(f / DYN_STEP) * DYN_STEP));
}
// the factor where G + W is predicted to fit `budget` (G scales with the area), on the grid
static double dyn_fit(double G, double W, double budget) {
    double fit = g_dyn * sqrt(fmax(0, budget - W) / G);
    double f = floor(fit / DYN_STEP + 1e-6) * DYN_STEP;
    return f < g_dyn_floor + DYN_STEP / 2 - 1e-6 ? g_dyn_floor : f;
}
// the floor for a Match display of `lines` render lines: Minimum resolution's lines (1 when the display has that many
// or fewer)
static void dyn_set_floor(double lines) {
    int m = g_settings.dyn_min;
    double f = fmin(1.0, (m == 360 || m == 720 ? m : 540) / fmax(lines, 1));
    if (fabs(f - g_dyn_floor) < 1e-9) return;
    g_dyn_floor = f;
    if (g_dyn < f) g_dyn = f;
    else g_dyn = dyn_grid(g_dyn);
}

// The window's size changed (fullscreen <-> windowed, a resize): every scalable target is reallocated and what was
// measured belongs to the old size. The factor keeps the render height (to the nearest step), so the game's part of
// the GPU time stays about the same. Before, the factor stayed as it was: a window made larger missed 60 until the
// slow frames stepped it down, one made smaller sat blurry until a raise, and a hold (30 fps on an integrated GPU)
// ended because the window step's time changed, so the factor went down to the floor and climbed back over the next
// seconds - the image changed sharpness for a while after every switch (issue #205). A hold now keeps its frame rate
// (its floor's G scaled to the new floor, the window step measured again, a probe for 60 fps a probe wait away); the
// votes start over, the raise wait is back at its first 5 s, and the next decision comes 1 s after the change, from
// frames at the new size only (a window dragged to size: none until it stops).
static void dyn_window_changed(double now, double from, double to) {
    double prev = g_dyn, prev_floor_lines = g_dyn_floor * from;
    dyn_set_floor(to);
    g_dyn = dyn_grid(prev * from / to);
    if (g_hold_dt > 0) {
        double r = g_dyn_floor * to / prev_floor_lines;
        g_floor_g *= r * r;
        g_ref_pending = true;  // (G's reference at the new size: the first decision there)
        g_hold_w = -1;         // (the window step's time at the new size: the first decision there)
        g_hold_miss = g_hold_up_votes = g_hold_met_votes = 0;
        g_probe_at = fmax(g_probe_at, now + g_probe_wait);  // (a probe for 60: once the hold has measured the new size)
    }
    g_down_hist = 0, g_down_fast_votes = 0, g_dyn_good = 0, g_prev_G = -1;
    g_chk_from = g_chain_from = -1, g_futile = 0;  // (g_paced stays: it's the GPU's)
    memset(g_down_gw, 0, sizeof g_down_gw);
    g_dyn_raise_wait = DYN_RAISE_WAIT, g_dyn_raise_at = now + DYN_RAISE_WAIT, g_dyn_last_raise = g_dyn_last_lower = -100;
    g_dyn_cool_until = now + 1.0;
    g_dyn_n = g_dyn_gpu_n = 0, g_dyn_gpu_sum = g_dyn_win_sum = 0;
    LOG("[dynres] window %.0f -> %.0f lines: %.2f -> %.2f (render height %.0f -> %.0f)%s\n", from, to, prev, g_dyn, prev * from,
        g_dyn * to, g_hold_dt > 0 ? ", the hold goes on" : "");
}

static bool g_win_setting;  // the upscaler or anti-aliasing setting changed (output_scale): a hold ends at the next decision
static DynSplit g_split;  // sums since the last decision
static int g_split_n;
void output_scale_note_split(const DynSplit *d) {
    if (g_split_n >= 600) { memset(&g_split, 0, sizeof g_split); g_split_n = 0; }  // (not deciding: Dynamic is off)
    g_split.game += d->game; g_split.win += d->win; g_split.span += d->span; g_split.frames += d->frames;
    g_split.w_slot += d->w_slot; g_split.w_acquire += d->w_acquire; g_split.w_present += d->w_present; g_split.w_other += d->w_other;
    g_split.win_counted = d->win_counted;
    g_split_n++;
}

static double g_sum_ms;  // (Metal) spans summed since the last decision
static int g_sum_n;
void output_scale_note_sum(double ms) {
    if (g_sum_n >= 600) g_sum_ms = 0, g_sum_n = 0;
    g_sum_ms += ms, g_sum_n++;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// game_ms: the game's GPU time of the last frame (< 0: unknown); win_ms: the window step's (< 0: unknown / not counted)
static void dyn_update(double now, double game_ms, double win_ms, double lines) {
    static int no_gpu = -1;
    if (no_gpu < 0) no_gpu = getenv("LARECOMP_DYN_NO_GPU") && atoi(getenv("LARECOMP_DYN_NO_GPU"));
    if (no_gpu) game_ms = -1;
    static double prev_lines;
    if (prev_lines > 0 && fabs(lines - prev_lines) > 0.5) dyn_window_changed(now, prev_lines, lines);
    prev_lines = lines;
    dyn_set_floor(lines);
    static int paced_init;
    if (!paced_init) {
        paced_init = 1;
        const char *e = getenv("LARECOMP_DYN_PACED");
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        g_paced = !e || atoi(e) != 0;
#else
        g_paced = e && atoi(e) != 0;
#endif
        if (g_paced) LOG("[dynres] paced from the start: down when frames come late, up when they're on time\n");
    }
    static int start = -1;  // LARECOMP_DYN_START=<f>: Dynamic starts at factor f (tests: climbing back from the floor)
    if (start < 0) {
        start = 0;
        if (getenv("LARECOMP_DYN_START")) g_dyn = dyn_grid(fmin(1.0, atof(getenv("LARECOMP_DYN_START"))));
    }
    // The menu open: no measures, no decisions, and none for DYN_MENU_SETTLE s after it closes - the factor stays where it
    // was. The menu is drawn in the window step (W), and the game keeps running behind it: on an iPad Pro M4 at 2420x1668
    // W went 0.9 -> 5 ms with it open, G + W 17.5 ms, frames a millisecond late, and Dynamic stepped 1.00 -> 0.55 behind
    // the menu. The menu is a moment, not the scene's load.
    extern bool menu_is_open(void);
    static bool menu_was_open;
    bool menu_open = menu_is_open();
    if (menu_open || menu_was_open) {
        g_last_t = now;
        g_dyn_n = g_dyn_gpu_n = 0, g_dyn_gpu_sum = g_dyn_win_sum = 0, g_sum_ms = 0, g_sum_n = 0;
        memset(&g_split, 0, sizeof g_split), g_split_n = 0;
        if (menu_open) {
            if (!menu_was_open) LOG("[dynres] menu open: held at %.2f\n", g_dyn);
            menu_was_open = true;
            return;
        }
        LOG("[dynres] menu closed: %.2f, deciding again in %.0f s\n", g_dyn, DYN_MENU_SETTLE);
        menu_was_open = false;  // (closed: the votes and checks from before it opened are old too)
        g_down_hist = 0, g_down_fast_votes = 0, g_dyn_good = 0, g_prev_G = -1;
        memset(g_down_gw, 0, sizeof g_down_gw);
        g_chk_from = g_chain_from = -1, g_futile = 0;
        g_hold_miss = g_hold_up_votes = g_hold_met_votes = 0, g_t30_late = 0;
        g_dyn_cool_until = fmax(g_dyn_cool_until, now + DYN_MENU_SETTLE);
        return;
    }
    double dt = g_last_t > 0 ? (now - g_last_t) * 1000.0 : 16.7;
    g_last_t = now;
    if (dt > 250) return;  // (loading, menus, a stall: not the GPU's steady state)
    if (g_dyn_n == 0) g_dec_t0 = now;
    g_dyn_dt[g_dyn_n++] = dt;
    if (game_ms >= 0) { g_dyn_gpu_sum += game_ms; g_dyn_win_sum += fmax(0, win_ms); g_dyn_gpu_n++; }
    if (g_dyn_n < DYN_NMAX && (g_dyn_n < 4 || now - g_dec_t0 < DYN_DEC_S)) return;  // decide every quarter second
    // frame times: median, and the mean without the outliers (single CPU-side spikes)
    int N = g_dyn_n;
    double sorted[DYN_NMAX], kept = 0;
    memcpy(sorted, g_dyn_dt, N * sizeof *sorted);
    qsort(sorted, N, sizeof *sorted, cmp_double);
    double med = N % 2 ? sorted[N / 2] : (sorted[N / 2 - 1] + sorted[N / 2]) / 2;
    int nkept = 0, nlate = 0;  // (nlate: frames shown a vsync late or more, 25 ms+, spikes aside)
    for (int i = 0; i < N; i++) if (sorted[i] <= 3 * med) { kept += sorted[i]; nkept++; nlate += sorted[i] >= 25.0; }
    bool gpu = g_dyn_gpu_n >= N / 2;
    double avg_dt = kept / nkept, G = gpu ? g_dyn_gpu_sum / g_dyn_gpu_n : -1, W = gpu ? g_dyn_win_sum / g_dyn_gpu_n : 0;
    double avg_gpu = gpu ? G + W : -1;
    int spikes = N - nkept;
    g_dyn_gpu_sum = g_dyn_win_sum = 0;
    g_dyn_n = g_dyn_gpu_n = 0;
    double sum_ms = g_sum_n ? g_sum_ms / g_sum_n : -1;
    g_sum_ms = 0, g_sum_n = 0;
    if (now < g_dyn_cool_until) return;
    double prev = g_dyn;
    static int log = -1;  // LARECOMP_LOG_DYNRES=1: every decision, not only the steps
    if (log < 0) log = getenv("LARECOMP_LOG_DYNRES") != NULL;
    double budget = DYN_BUDGET;
    bool hold_up = false;  // (20 -> 30 fps this time: the factor was set to fit it already)
    // Internal FPS target 30 (pace_target_30: the setting, or outdoors with "30 outdoors, 60 indoors"): held at 30 from
    // the start - the factor only goes down when even 30 doesn't fit, and 60 is never looked for. When the target goes
    // back to 60 (indoors, or the setting changed) that hold ends and Dynamic aims for 60 again, as it would have.
    extern bool pace_target_30(void);
    static bool was_t30;
    bool t30 = pace_target_30();
    if (gpu && t30 && hold_vsyncs() < 2) {
        LOG("[dynres] Internal FPS target 30: holding 30 fps (game %.1f + window %.1f ms at %.2f)\n", G, W, g_dyn);
        g_hold_dt = 2 * 1000 / 60.0, g_hold_w = W;
        g_floor_g = g_ref_g = G, g_ref_pending = false;
        g_probing = false, g_hold_miss = g_hold_up_votes = g_hold_met_votes = 0;
        g_t30_cap = 0, g_t30_at = now, g_t30_late = 0, g_t30_wait = 4, g_t30_off_at = -1e9, g_t30_limits = false;
    } else if (was_t30 && !t30 && g_hold_dt > 0) {
        LOG("[dynres] Internal FPS target 60 here: hold at %.0f fps ended\n", 1000 / g_hold_dt);
        g_hold_dt = 0, g_probing = false, g_hold_miss = g_hold_up_votes = g_hold_met_votes = 0;
    }
    was_t30 = t30;
    bool win_setting = g_win_setting;
    g_win_setting = false;
    if (gpu) {
        if (g_hold_dt > 0 && g_hold_w < 0) g_hold_w = W;  // (the window's size changed: its step's time there)
        else if (g_hold_dt > 0 && t30 && fabs(W - g_hold_w) > fmax(1.0, 0.25 * g_hold_w))
            g_hold_w = W;  // (target 30: the hold is the setting's, not the window step's - the menu opening and closing
                           // moves W 1 -> 6 -> 2 ms; ending the hold judged that half second by 60's budget: 1.00 -> 0.70)
        // Another upscaler / anti-aliasing (output_scale: the setting itself): the hold was learned with the old window step.
        // It went by W's time before, 25% off ending it - but an upscaler's step grows with its input (FSR 1 on a Retroid
        // Pocket 5: 4.7 ms at 0.33, 6.6 at 0.85), so a hold climbing as 30 allows ended itself, was judged by 60's budget
        // (0.85 -> 0.55), held again and climbed again, for good
        else if (win_setting && g_hold_dt > 0) {
            LOG("[dynres] window step setting changed: hold at %.0f fps ended\n", 1000 / g_hold_dt);
            g_hold_dt = 0, g_probing = false, g_probe_wait = 10.0, g_hold_miss = g_hold_up_votes = g_hold_met_votes = 0;
        }
        // held at 20 while the frames already come at 30 (every 33.3 ms) with G + W inside the 30 hold's budget (10%
        // to spare), 1 s in a row: it holds 30 again right away (no probe wait - unless the last step up was undone)
        int hvs = g_hold_dt > 0 ? hold_vsyncs() : 0;
        double faster = (hvs - 1) * 1000 / 60.0;
        bool met = hvs > 2 && avg_dt <= 1.05 * faster && avg_gpu <= faster * 0.87 * DYN_RAISE_FIT;
        g_hold_met_votes = met ? g_hold_met_votes + 1 : 0;
        if (g_hold_dt > 0 && g_ref_pending) g_ref_g = G, g_ref_pending = false;  // (the first measure at a new factor)
        if (g_hold_dt > 0 && g_ref_g > 0) {
            // the next faster rate: 20 -> 30 fps, 30 -> looking for 60; the floor must fit it with room, 2 s in a row
            double g_floor = g_floor_g * G / g_ref_g;
            int vs = hvs;
            double up_budget = vs > 2 ? (vs - 1) * 1000 / 60.0 * 0.87 : DYN_BUDGET;
            bool room = g_floor + W <= up_budget * DYN_HOLD_UP_FIT;
            g_hold_up_votes = room ? g_hold_up_votes + 1 : 0;
            bool met_now = met && g_hold_met_votes >= 4 && (g_probe_wait <= 10.0 || now >= g_probe_at);
            if (((room && g_hold_up_votes >= 8 && now >= g_probe_at) || met_now) && !(t30 && vs <= 2)) {  // (30 target: not above 30)
                g_hold_up_votes = g_hold_miss = g_hold_met_votes = 0;
                if (vs > 2) {
                    g_hold_dt = (vs - 1) * 1000 / 60.0;
                    g_hold_up_at = now, g_probe_at = now + g_probe_wait;
                    double to = fmin(g_dyn, dyn_fit(G, W, g_hold_dt * 0.87 * DYN_RAISE_FIT));
                    if (met_now)
                        LOG("[dynres] frames already meet %.0f fps (every %.1f ms, game %.1f + window %.1f ms at %.2f): holding "
                            "%.0f fps\n", 1000 / g_hold_dt, avg_dt, G, W, g_dyn, 1000 / g_hold_dt);
                    else
                        LOG("[dynres] the floor would fit %.0f fps now (game %.1f ms at %.2f: ~%.1f at %.2f, window %.1f): holding "
                            "%.0f fps, %.2f -> %.2f\n", 1000 / g_hold_dt, G, g_dyn, g_floor, g_dyn_floor, W, 1000 / g_hold_dt, g_dyn, to);
                    if (to != g_dyn) g_floor_g *= G / g_ref_g, g_ref_pending = true, g_dyn = to;
                    hold_up = true;
                } else {
                    LOG("[dynres] the floor would fit 60 fps now (game %.1f ms at %.2f: ~%.1f at %.2f, window %.1f): looking for it\n",
                        G, g_dyn, g_floor, g_dyn_floor, W);
                    g_hold_dt = 0, g_probing = true, g_probe_start = now, g_probe_good = 0;
                    pace_try_60();  // (the game's 30 fps mode, if the frame pacer holds it, ends too: pace.c)
                }
            }
        }
        // Held at 30 with the GPU 30% idle or more: G may be inflated by a lowered clock (an integrated GPU clocks down
        // while frames wait for the display: UHD 770, 1080p window, G 15.9 ms at 0.50 held at 30 against 9-11 at 60),
        // so the hold's own measure may never show 60 fitting again (it stayed at 30 for the rest of a run). Idle by the
        // floor's estimate, not the GPU's time now: held, the factor climbs while 30 fits (G + W up to 26 ms), so the GPU
        // looked busy and no probe came (Retroid Pocket 5 at 0.85). The probe ends the frame pacer's hold of the game's
        // 30 fps mode too (pace_try_60): it only lowered the resolution before, the game stayed at 30 and every probe
        // failed (pace.c). Every probe
        // wait (10 s, doubling each time a probe fails, at most a minute) it tries 60 at the floor; a probe that fails is
        // back in the hold within ~1.5 s at the factor it had, with frames that were at 30 anyway. After a failed probe
        // the next one waits until the floor's estimate is 10% under what that probe measured there (the scene got
        // lighter) or 2 minutes have passed: a hold that is real (SMAA + FSR 1 at 1080p on the UHD 770) probes rarely.
        double floor_gw = g_ref_g > 0 ? g_floor_g * G / g_ref_g + W : -1;
        // (the 2 minutes only where the failed probe came near: G + W within 15% of the budget. A hold 60 can't reach -
        // SMAA + FSR 1 at 1080p - probed every 2 minutes for good, each probe a second or two of the game's 60 fps mode at
        // 30 fps: slow motion)
        bool probe_ok = (now - g_probe_fail_at > 120.0 && g_probe_fail_gw <= 1.15 * DYN_BUDGET) || (floor_gw > 0 && floor_gw < 0.9 * g_probe_fail_gw);
        double idle_gw = floor_gw > 0 ? floor_gw : avg_gpu;
        if (g_hold_dt > 0 && !hold_up && !t30 && hold_vsyncs() == 2 && idle_gw < 0.7 * g_hold_dt && now >= g_probe_at && probe_ok) {
            LOG("[dynres] held at 30 with the GPU %.0f%% busy at the floor (game %.1f + window %.1f ms at %.2f): its clock may "
                "be down, trying 60 at %.2f\n", 100 * idle_gw / g_hold_dt, G, W, g_dyn, g_dyn_floor);
            pace_try_60();
            g_hold_dt = 0, g_probing = true, g_probe_start = now, g_probe_good = 0;
            g_hold_up_votes = g_hold_miss = g_hold_met_votes = 0;
            g_probe_from = g_dyn, g_dyn = g_dyn_floor;
        }
        if (g_hold_dt > 0) budget = g_hold_dt * 0.87;
        // Target 30: the frames themselves must come every 33.3 ms. G + W fitting the hold's budget isn't enough where the
        // CPU's part of a frame doesn't overlap the GPU's (Retroid Pocket 5, Mabe Village, the runtime built -O0: G + W 19
        // ms at 1.00 under a 29 ms budget, frames every 39-50 ms). Frames slower than the hold with the GPU a real part of
        // them, 1.5 s in a row, cap the game's part (G) just under what it was: the factor steps down one step (another
        // after 1.5 s more of late frames) and raises stay under it. 10 s of frames on time lifts the cap (the normal raises take it back up); late again within 30 s of that, the
        // next lift waits twice as long (at most 2 minutes; back to 10 s after a minute without a cap). It took one late
        // half second and eased 5% every 30 s: the frames after a warp (shaders compiling, 35.5 ms with G + W 16 ms) kept
        // R6 on the Retroid at 0.60 for minutes where 1.00 holds 30.
        g_t30_limits = false;
        if (t30 && g_hold_dt > 0 && hold_vsyncs() == 2) {
            bool late = avg_dt > 1.06 * g_hold_dt && !spikes && avg_gpu > 0.4 * g_hold_dt;
            g_t30_late = late ? g_t30_late + 1 : 0;
            if (late && g_t30_late >= 3 && now >= g_dyn_cool_until) {
                double cap = 0.92 * G;  // (the game's part: the window step's grows below 1.00 - the upscaler - and a cap
                                        // on G + W set at 1.00 took a second step down for nothing)
                if (g_t30_cap <= 0) {
                    g_t30_wait = now - g_t30_off_at < 15.0 ? fmin(2 * g_t30_wait, 60.0) : now - g_t30_off_at > 30.0 ? 4.0 : g_t30_wait;
                    LOG("[dynres] Internal FPS target 30: frames every %.1f ms with game %.1f + window %.1f ms at %.2f: "
                        "game kept under %.1f ms (lifted after %.0f s on time)\n", avg_dt, G, W, g_dyn, cap, g_t30_wait);
                } else if (cap < g_t30_cap)
                    LOG("[dynres] Internal FPS target 30: frames every %.1f ms with game %.1f + window %.1f ms at %.2f: "
                        "game kept under %.1f ms\n", avg_dt, G, W, g_dyn, cap);
                g_t30_cap = g_t30_cap > 0 ? fmin(g_t30_cap, cap) : cap, g_t30_at = now, g_t30_late = 0;
            } else if (late || avg_dt > 1.02 * g_hold_dt)
                g_t30_at = now;  // (the lift waits for frames on time)
            else if (g_t30_cap > 0 && now - g_t30_at >= g_t30_wait) {
                LOG("[dynres] Internal FPS target 30: frames on time for %.0f s, the cap (game %.1f ms) lifted\n", g_t30_wait, g_t30_cap);
                g_t30_cap = 0, g_t30_off_at = now;
            }
            g_t30_limits = g_t30_cap > 0 && g_t30_cap + W < budget;
            if (g_t30_limits) budget = g_t30_cap + W;
        }
        // 60 out of reach at the floor and the frames wait for the display: lower gains nothing, hold that frame rate
        // (whole refreshes of a 60 Hz display: a floor that alternates 1 and 2 of them - 40 fps - beats 30 at a higher
        // factor; at most 2 refreshes, 30 fps: DYN_HOLD_MAX)
        double vsyncs = fmin(floor(avg_dt / (1000 / 60.0) + 0.1), DYN_HOLD_MAX);
        static int votes;  // (a second in a row)
        bool held_back = g_hold_dt == 0 && at_floor() && avg_gpu > DYN_BUDGET && vsyncs >= 2 && avg_dt >= 1.25 * avg_gpu &&
                         now - g_probe_start >= 1.5;  // (a probe: the clock comes back up first)
        votes = held_back ? votes + 1 : 0;
        if (held_back && votes >= 4) {
            g_hold_dt = vsyncs * 1000 / 60.0, g_hold_w = W;
            g_floor_g = g_ref_g = G, g_ref_pending = false;
            g_hold_miss = g_hold_up_votes = g_hold_met_votes = 0;
            // a probe that failed, or one met a moment ago that didn't last: wait longer for the next
            if (g_probing || now - g_probe_ok_at < 30.0) g_probe_wait = fmin(g_probe_wait * 2, DYN_PROBE_MAX);
            // (a probe is judged within 8 s: the pacer's second of grace, its 60 frames, then this hold's second)
            if (g_probing && now - g_probe_start < 8.0) g_probe_fail_gw = G + W, g_probe_fail_at = now;
            if (g_probing && now - g_probe_start < 8.0 && g_probe_from > g_dyn) {  // (a probe failed: the hold's factor again)
                LOG("[dynres] 60 fps didn't fit: back to %.2f\n", g_probe_from);
                g_ref_pending = true;  // (the floor's G as just measured; G's reference at that factor next)
                g_dyn = g_probe_from;
            }
            g_probing = false;
            g_probe_at = now + g_probe_wait;
            budget = g_hold_dt * 0.87;
            if (W >= 0.4 * 1000 / 60.0 || W >= G)
                LOG("[dynres] window step %.1f ms is the limit (game %.1f ms at %.2f, a frame every %.1f ms): 60 fps out of "
                    "reach, holding %.0f fps; the factor goes up while G + W fits %.1f ms\n", W, G, g_dyn, avg_dt,
                    1000 / g_hold_dt, budget);
            else
                LOG("[dynres] game %.1f + window %.1f ms at %.2f (a frame every %.1f ms): 60 fps out of reach, holding %.0f "
                    "fps; the factor goes up while G + W fits %.1f ms\n", G, W, g_dyn, avg_dt, 1000 / g_hold_dt, budget);
        }
        // a probe met 60: two decisions in a row (one could be the game's own frames before its 30 fps mode ends)
        g_probe_good = g_probing && g_hold_dt == 0 && avg_gpu <= DYN_BUDGET && avg_dt < 20 ? g_probe_good + 1 : 0;
        if (g_probe_good >= 4) g_probing = false, g_probe_good = 0, g_probe_ok_at = now, g_probe_wait = 10.0;
    }
    // the last step down's check (see g_paced): its first measure
    if (g_chk_from > 0 && g_dyn == g_chk_from) g_chk_from = -1;  // (undone already)
    if (g_chk_from > 0 && gpu && g_hold_dt == 0 && !spikes) {
        double pred_gain = g_chk_gw - g_chk_pred, gain = g_chk_gw - avg_gpu;
        // (frames on time: at most one late - a hitch in the check's quarter second said nothing and reset the count, and
        // a Retroid took ~55 s and a menu to be found paced)
        bool on_time = nlate <= 1;
        bool futile = pred_gain >= 0.5 && gain < DYN_CHECK_SHARE * pred_gain && on_time;
        if (futile) g_futile++, g_futile_at = now;
        else if (on_time) g_futile = 0;
        if (futile && g_futile < 2)
            LOG("[dynres] %.2f -> %.2f saved %.1f of a predicted %.1f ms (gpu %.1f -> %.1f ms = game %.1f + window %.1f, frames every %.1f ms)\n",
                g_chk_from, g_dyn, gain, pred_gain, g_chk_gw, avg_gpu, G, W, avg_dt);
        if (futile && g_futile >= 2) {
            double back = fmax(g_chain_from, g_chk_from);
            LOG("[dynres] %.2f -> %.2f saved %.1f of a predicted %.1f ms (gpu %.1f -> %.1f ms = game %.1f + window %.1f, frames "
                "every %.1f ms): the GPU's clock follows the load, back to %.2f; from now on down only when frames come late\n",
                g_chk_from, g_dyn, gain, pred_gain, g_chk_gw, avg_gpu, G, W, avg_dt, back);
            g_paced = true, g_dyn = back, g_chk_from = g_chain_from = -1, g_futile = 0;
            g_down_hist = 0, g_down_fast_votes = 0, g_dyn_good = 0;
            memset(g_down_gw, 0, sizeof g_down_gw);
            g_dyn_last_lower = -100, g_dyn_raise_wait = DYN_RAISE_WAIT, g_dyn_raise_at = now + DYN_RAISE_WAIT;
            g_dyn_last_raise = now;  // (late frames right after: back one step, not straight down - a raise)
            goto decided;
        }
        g_chk_from = -1;
    }
    bool slow = gpu ? avg_gpu > budget : avg_dt > 17.4;
    // paced (see g_paced): the GPU's time is near the budget at any factor; late frames with the GPU busy are what count.
    // Late frames a run of them, not one: a GPU too slow for the factor makes many frames late, a hitch (the CPU's, a
    // stream-in) one now and then - and with the GPU always ~90% busy here each of those read as the GPU's. On a Retroid
    // Pocket 5 (Turnip at 60 fps: a 35 ms frame every few seconds) every one stepped it down, each raise was undone
    // within 3 s (the raise wait doubling to 16 s), and it sank to its floor at 60 fps throughout - 486p for minutes.
    bool paced = gpu && g_paced && g_hold_dt == 0;
    // (and a late frame now with 3 or more in the last second: a factor a little over makes one late every few frames,
    // under 2 a quarter second - it sat at 0.80 at 55 fps for 46 s)
    static int late_hist[4];  // late frames of the last 4 decisions (a second)
    memmove(late_hist + 1, late_hist, 3 * sizeof *late_hist), late_hist[0] = nlate;
    int late4 = late_hist[0] + late_hist[1] + late_hist[2] + late_hist[3];
    if (paced) slow = ((nlate >= 2 && nlate * 6 >= N) || (nlate >= 1 && late4 >= 3)) && avg_gpu > 0.8 * budget;
    // held, but the frames came slower than the hold's (the display waits one more refresh): over budget too
    if (gpu && g_hold_dt > 0 && avg_dt > 1.2 * g_hold_dt && avg_gpu > 0.8 * budget) slow = true;
    bool hold_wait = hold_up;  // held and over its budget, not yet long enough to act on it (or the hold just stepped up)
    if (gpu && g_hold_dt > 0) {
        g_hold_miss = slow && !hold_up ? g_hold_miss + 1 : 0;
        // nothing lower: the hold steps down to a slower rate, up to DYN_HOLD_MAX (30: so never - the floor is the best
        // it can do for 30)
        if (slow && at_floor() && g_hold_miss >= 6) {
            int vs = hold_vsyncs();
            if (vs < DYN_HOLD_MAX) {
                LOG("[dynres] game %.1f + window %.1f ms at %.2f misses the %.0f fps hold's %.1f ms (a frame every %.1f ms): "
                    "holding %.0f fps\n", G, W, g_dyn, 1000 / g_hold_dt, budget, avg_dt, 60.0 / (vs + 1));
                if (now - g_hold_up_at < 30.0) g_probe_wait = fmin(g_probe_wait * 2, DYN_PROBE_MAX);  // (the step up didn't hold)
                g_hold_dt = (vs + 1) * 1000 / 60.0;
                budget = g_hold_dt * 0.87;
                g_floor_g = g_ref_g = G, g_ref_pending = false;
                g_probe_at = now + g_probe_wait;
                g_hold_up_votes = 0;
            }
            g_hold_miss = 0;
            hold_wait = true;
        } else if (slow && (at_floor() || g_hold_miss < 4)) hold_wait = true;
        if (slow && !hold_wait) g_hold_miss = 0;
    }
    // over budget for a while (not held: a hold acts on its own misses above): 3 of the last 4 decisions, or their G + W
    // averaging over 95% of the budget (a load near the budget alternates over / under from one half second to the
    // next while frames already miss: M3 Max at 7680x4320, G + W 12 / 15.5 / 12.4 / 15.9 ms, frames every 17-19 ms).
    // Frames missing vsync often (a frame every 20 ms on average) with the GPU near the budget count as over too. The
    // frames missing 60 outright (G + W over a whole refresh) acts sooner.
    bool must = false;
    if (g_hold_dt == 0 && !hold_wait) {
        // (not while the frame pacer holds the game at 30 with G + W inside 60's budget: the frames come every 33 ms for the
        // pacer's reasons - the CPU, the window's image, the GPU's lowered clock - and a lower factor doesn't change that.
        // A Retroid stepped 0.45 -> 0.40 -> 0.33 there with the GPU at 12 ms)
        extern bool pace_is_slow(void);
        bool over = slow || (gpu && avg_dt > 20.0 && avg_gpu > (pace_is_slow() ? budget : 0.8 * budget));
        if (over && !slow) slow = true;  // (the steps below go by `slow`)
        g_down_hist = (g_down_hist << 1 | over) & 15;
        memmove(g_down_gw + 1, g_down_gw, 3 * sizeof *g_down_gw);
        g_down_gw[0] = gpu ? avg_gpu : 0;
        double mean4 = (g_down_gw[0] + g_down_gw[1] + g_down_gw[2] + g_down_gw[3]) / 4;
        bool missing = gpu ? avg_gpu > budget / 0.87 : avg_dt > 20.0;
        g_down_fast_votes = missing ? g_down_fast_votes + 1 : 0;
        must = __builtin_popcount(g_down_hist) >= DYN_DOWN_VOTES || g_down_fast_votes >= DYN_DOWN_FAST ||
               (slow && g_down_gw[3] > 0 && mean4 > 0.95 * budget);
    }
    if (slow && !at_floor() && g_hold_dt == 0 && !hold_wait && !must) g_dyn_good = 0;  // (over: waiting to be sure)
    else if (hold_wait || (slow && at_floor())) g_dyn_good = 0;  // (at the floor: nothing lower)
    else if (slow) {
        double down = g_dyn - DYN_STEP;
        if (gpu && G > 0) {  // straight to where G + W is predicted to fit, with the room a raise would need (G and W of
                             // the last second: one half second's peak would overshoot it, and a raise would follow)
            double g2 = g_prev_G > 0 ? (G + g_prev_G) / 2 : G, w2 = g_prev_G > 0 ? (W + g_prev_W) / 2 : W;
            down = fmin(down, g_hold_dt > 0 ? dyn_fit(g2, w2, budget * DYN_RAISE_FIT) : dyn_fit(g2 / DYN_UP_FIT, w2, budget));
            down = fmax(down, g_dyn - DYN_MAX_DOWN);
            // a little over (or target 30's frames a little late): one small step - the next decision a quarter second
            // later takes another if it's still over; far over: straight to where it fits
            // (paced: a raise that made the frames late - the step before it fit)
            // (paced: always one step - the G it would fit is over-read, and the ceiling keeps raises under it)
            if (g_t30_limits || avg_gpu <= DYN_DOWN_SMALL * budget || paced) down = g_dyn - DYN_STEP;
        }
        if (paced) {  // the factor that went late: the ceiling (see g_pc_ceil)
            if (now < g_pc_until + 30.0) g_pc_wait = fmin(g_pc_wait * 2, 60.0);  // (hit again soon after a lift)
            else if (now - g_pc_hit_at > 120.0) g_pc_wait = 10.0;
            g_pc_ceil = g_dyn, g_pc_until = now + g_pc_wait, g_pc_hit_at = now;
            g_pc_ref_f = fmax(g_dyn_floor, g_dyn - DYN_STEP), g_pc_ref_g = -1;  // (G one step under: the first on-time measure there)
            LOG("[dynres] paced: frames late at %.2f, raises stay under it for %.0f s\n", g_dyn, g_pc_wait);
        }
        g_dyn = fmax(g_dyn_floor, down);
        g_dyn_good = 0;
        g_down_hist = 0, g_down_fast_votes = 0;
        memset(g_down_gw, 0, sizeof g_down_gw);
        // the raise didn't hold (spikes can't undo one any more: they're not in the measure)
        // (paced: the ceiling waits instead - the climb under it stays a step a second)
        if (now - g_dyn_last_raise < 10.0 && !paced) g_dyn_raise_wait = fmin(g_dyn_raise_wait * 2, DYN_RAISE_WAIT_MAX);
        g_dyn_last_lower = now;
        g_dyn_raise_at = now + g_dyn_raise_wait;
    } else if (g_dyn < 1.0) {
        // headroom for a while (every decision of it), and not too soon after the last change. With GPU timing: the
        // next step is predicted to fit with room (G scales with the area); without: frames at 60
        double up = dyn_grid(g_dyn + DYN_STEP);
        double g_up = G * (up / g_dyn) * (up / g_dyn);
        bool fits = !gpu ? avg_dt < 16.9 : paced ? late4 <= 1 && med < 17.2 : g_hold_dt > 0 ? g_up + W <= budget * DYN_RAISE_FIT : g_up / DYN_UP_FIT + W <= budget;
        // (a decision with spikes - a level loading, a fade, a cutscene starting - says little about the scene ahead:
        // it starts the count over, so a raise doesn't come from a light loading screen)
        g_dyn_good = fits && !spikes ? g_dyn_good + 1 : 0;
        bool room = fits && g_dyn_good >= DYN_UP_VOTES && now >= g_dyn_raise_at;
        if (paced && fabs(g_dyn - g_pc_ref_f) < 1e-6 && nlate == 0 && !spikes) {  // (the scene got lighter: lift it now)
            if (g_pc_ref_g < 0) g_pc_ref_g = G;
            else if (G < 0.9 * g_pc_ref_g && now < g_pc_until) { g_pc_until = now; LOG("[dynres] paced: game %.1f -> %.1f ms at %.2f, the ceiling %.2f is lifted\n", g_pc_ref_g, G, g_dyn, g_pc_ceil); }
        }
        if (paced && up >= g_pc_ceil - 1e-6 && now < g_pc_until) room = false;
        if (room) {
            g_dyn = up, g_dyn_good = 0, g_down_hist = 0, g_dyn_last_raise = now, g_dyn_raise_at = now + g_dyn_raise_wait;
            memset(g_down_gw, 0, sizeof g_down_gw);
        }
        else if (now - g_dyn_last_lower > 30.0 && g_dyn_raise_wait > DYN_RAISE_WAIT) g_dyn_raise_wait = DYN_RAISE_WAIT;  // (30 s of calm: forget the hunting)
    }
    g_dyn = dyn_grid(g_dyn);
    // (in a row: no step between them that saved what it should, and no raise - however far apart. A clock that fills
    // the frame to just under the budget steps down rarely: a Retroid's futile steps came 46 s apart, each counted as a
    // first, and it sank one step a minute to 432p at 60 fps. 2 minutes apart: unrelated)
    if (g_futile && (now - g_futile_at > 120.0 || g_dyn > prev)) g_futile = 0;
    if (g_dyn > prev || (g_dyn < prev && nlate > 1) || now - g_chain_at > 5.0) g_chain_from = -1;
    if (g_dyn < prev && gpu && g_hold_dt == 0 && !g_paced && !g_t30_limits && nlate <= 1) {  // (checked at the next decision)
        if (g_chain_from < 0) g_chain_from = prev;
        g_chain_at = now;
        double g2 = g_prev_G > 0 ? (G + g_prev_G) / 2 : G, w2 = g_prev_G > 0 ? (W + g_prev_W) / 2 : W;
        g_chk_from = prev, g_chk_gw = g2 + w2, g_chk_pred = g2 * (g_dyn / prev) * (g_dyn / prev) + w2;
    }
decided:
    g_prev_G = gpu && g_dyn == prev ? G : -1, g_prev_W = W;  // (a change: the next decision measures the new factor)
    if (gpu && g_hold_dt > 0 && g_dyn != prev && !g_ref_pending && g_ref_g > 0)  // the floor's G as of now; G's new reference next time
        g_floor_g *= G / g_ref_g, g_ref_pending = true;
    if (g_dyn != prev || log) {
        if (g_dyn != prev) g_dyn_cool_until = now + 0.25;  // (the first frames at the new scale)
        char split[320] = "";
        if (g_split_n) {  // (Vulkan) per frame: GPU by part, and where the CPU waited
            double n = g_split_n, nf = g_split.frames > 0 ? g_split.frames : n;
            snprintf(split, sizeof split, "; per frame: gpu game %.1f + window %.1f%s, span %.1f, gap %.1f ms; cpu waits slot %.1f acquire %.1f "
                     "present %.1f other %.1f ms", g_split.game / n, g_split.win / n, g_split.win_counted ? "" : " (not counted)",
                     g_split.span / nf, g_split.span / nf - (g_split.game + g_split.win) / n, g_split.w_slot / n, g_split.w_acquire / n,
                     g_split.w_present / n, g_split.w_other / n);
        }
        if (sum_ms >= 0) snprintf(split, sizeof split, "; spans summed %.1f ms", sum_ms);
        char hold[48] = "";
        if (g_hold_dt > 0) snprintf(hold, sizeof hold, ", holding %.0f fps", 1000 / g_hold_dt);
        LOG("[dynres] %.2f -> %.2f (frame %.1f ms, median %.1f, %d spike%s left out, gpu %.1f ms = game %.1f + window %.1f, "
            "budget %.1f%s, next raise in %.0f s%s)\n", prev, g_dyn, avg_dt, med, spikes, spikes == 1 ? "" : "s", avg_gpu, G, W,
            budget, hold, fmax(0, g_dyn_raise_at - now), split);
    }
    memset(&g_split, 0, sizeof g_split);
    g_split_n = 0;
}

double output_scale_dynamic_factor(void) { return g_dyn; }
static volatile int g_render_w, g_render_h;  // (the last frame's render size: the FPS counter's, menu.mm)
void output_scale_render_size(int *w, int *h) { *w = g_render_w, *h = g_render_h; }
double output_scale_dynamic_hold_fps(void) { return g_hold_dt > 0 ? 1000 / g_hold_dt : 0; }

// The game's ambient occlusion is drawn at a quarter of 1080p (480x270, then blurred, and a 240x135 level) and kept at
// that size at every render scale (issue #356: scaled with it, its sample pattern fell apart - see metal.m tex_ao).
// Stretched over a large frame it is soft and flickers at the edges of objects (issue #602). High draws it at the size
// of the depth it is made from (the depth pyramid's 960x540 level, at the render scale) - twice the Switch's at 1x and
// up, at four times the AO's cost (a sixteenth of a 4K frame's pixels) - and never below the Switch's size. The
// shaders are told the real pixel grid (maxwell/translate.c ao_grid_fix). By the allocation scale `al`, which Dynamic
// resolution keeps (no reallocation at its steps). LARECOMP_AO_SCALE=<n> (test): n times the Switch's size.
float output_scale_ao(float al) {
    static int mode = -1;  // (-1: unread, 0: the setting, 1: scaled with the frame, 2: LARECOMP_AO_SCALE)
    static float test;
    if (mode < 0) {
        const char *e = getenv("LARECOMP_AO_SCALE");
        test = e ? (float)atof(e) : 0;
        mode = getenv("LARECOMP_AO_SCALED") ? 1 : test >= 1 && test <= 4 ? 2 : 0;
    }
    if (mode == 1) return 0;
    if (mode == 2) return test;
    return g_settings.ao_quality == AO_HIGH ? fminf(2, fmaxf(1, 2 * al)) : 1;
}

// The game's shadow map: one 2048x2048 D24S8 render target for the whole scene (2D, not multisampled). Lit shaders take
// four compare taps one texel apart, in the game's texel size (maxwell/translate.c shadow_taps).
bool output_scale_is_shadow_map(uint32_t target, uint32_t samples, uint32_t w, uint32_t h, uint32_t format, bool render_target) {
    return target == 1 && samples <= 1 && w == 2048 && h == 2048 && format == 0x35 && render_target;
}

// Its scale: half to 4 times the Switch's size (the Shadows setting), at any render scale. Each step up costs more
// memory: about 60 MB at 2x and 300 MB at 4x; 0.5x saves 12 MB and a quarter of the pass's pixels, for weak GPUs.
// LARECOMP_SHADOW_SCALE=<n> (test, 0.5 to 4): n times the Switch's size.
float output_scale_shadow(void) {
    static float test = -1;
    if (test < 0) {
        const char *e = getenv("LARECOMP_SHADOW_SCALE");
        test = e ? (float)atof(e) : 0;
    }
    if (test >= 0.5f && test <= 4) return test;
    int q = g_settings.shadow_quality;
    return q == SHADOW_HALF ? 0.5f : q >= SHADOW_2X && q <= SHADOW_4X ? (float)(q - SHADOW_DEFAULT + 1) : 1;
}

// The polygon offset of draws into the shadow map: the game's slope factor counts in its map's texels, so with the map
// n times the Switch's size the same factor pushes casters back 1/n as far, and thin casters (grass cards, the stones'
// edges) shadow themselves - dark streaks at 4x. The factor is multiplied by the map's scale, the game's offset in the
// scene again. LARECOMP_SHADOW_BIAS_RAW=1 (A/B tests): the game's factor as it is.
float output_scale_shadow_slope(void) {
    static int raw = -1;
    if (raw < 0) raw = getenv("LARECOMP_SHADOW_BIAS_RAW") != NULL;
    return raw ? 1 : output_scale_shadow();
}

// Shadows Off: the draws into the shadow map are skipped (renderer.m / vulkan/exec.c draw). The game still clears it to
// 1.0 every frame, which its lit shaders' compare taps read as unshadowed, so nothing is shaded.
bool output_scale_shadow_off(void) {
    return g_settings.shadow_quality == SHADOW_OFF;
}

void output_scale_log(unsigned long long frame, double dw, double dh, const OutScale *o) {
    static double psx, psy, pdw, pdh;
    static int nlog;
    extern void pace_hitch(const char *why);  // pace.c: new targets at the new size, a stall
    if (psx && (o->sx != psx || o->sy != psy || dw != pdw || dh != pdh)) pace_hitch("a render scale change");
    if ((o->sx != psx || o->sy != psy || dw != pdw || dh != pdh) && nlog++ < 200)
        LOG("[scale] frame %llu drawable %.0fx%.0f -> render scale %.3fx%.3f (reshape %d)\n", frame, dw, dh, o->sx, o->sy,
            o->shape != 1.0);
    psx = o->sx, psy = o->sy, pdw = dw, pdh = dh;
}

void output_scale(double dw, double dh, bool reshape_allowed, double now, double game_ms, double win_ms, OutScale *o) {
    double fit = fmin(dw / 1920.0, dh / 1080.0);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("LARECOMP_SCALE") ? atof(getenv("LARECOMP_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("LARECOMP_WIDE_TEST") ? atof(getenv("LARECOMP_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (16.0 / 9.0);
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005;
    if (!reshape) shape = 1.0;
    // render height in units of the Switch's 1080 lines
    double h, dyn = 1.0;
    int mode = g_settings.res_mode;
    // The user changed a setting that changes the GPU's load (the menu, the start screen; Dynamic's own steps are g_dyn,
    // not a setting): the frame pacer's 30 fps hold and backoff were learned under the old load (pace.c).
    static int p_mode = -1, p_ups, p_aa, p_fps;
    if (p_mode >= 0) {
        extern void pace_settings_changed(const char *what);
        if (mode != p_mode) pace_settings_changed("resolution");
        else if (g_settings.upscaler != p_ups) pace_settings_changed("upscaler"), g_win_setting = true;
        else if (g_settings.antialias != p_aa) pace_settings_changed("anti-aliasing"), g_win_setting = true;
        else if (g_settings.fps_target != p_fps) pace_settings_changed("Internal FPS target");
    }
    p_mode = mode, p_ups = g_settings.upscaler, p_aa = g_settings.antialias, p_fps = g_settings.fps_target;
    if (forced > 0) h = forced;
    else if (mode >= 1 && mode <= 8) h = mode == 1 && g_gpu_integrated ? 720 / 1080.0 : mode;
    else {
        h = reshape ? dh / 1080.0 : fit;  // Match display
        if (mode == RES_DYNAMIC) {
            static double pinned = -1;
            if (pinned < 0) pinned = getenv("LARECOMP_DYN_TEST") ? atof(getenv("LARECOMP_DYN_TEST")) : 0;
            if (pinned > 0) dyn = pinned;
            else { dyn_update(now, game_ms, win_ms, h * 1080); dyn = g_dyn; }
        }
        else if (mode == RES_Q75) h *= 0.75;
        else if (mode == RES_Q25) h *= 0.25;
        else if (mode == RES_Q50) h *= 0.5;
    }
    h = fmax(h, 0.1);  // (.25x of a small window)
    double ax = h * shape, ay = h;
    double cap = fmin(MAX_TEX / (1920.0 * ax), MAX_TEX / (1080.0 * ay));
    if (cap < 1.0) { ax *= cap; ay *= cap; }
    o->ax = ax; o->ay = ay;
    o->sx = ax * dyn; o->sy = ay * dyn; o->shape = shape;
    g_render_w = (int)lround(1920.0 * o->sx), g_render_h = (int)lround(1080.0 * o->sy);
    // where the image goes in the window: fills it when reshaped, else a fitted 16:9 box
    if (reshape) { o->vw = dw; o->vh = dh; }
    else { o->vw = 1920.0 * fit; o->vh = 1080.0 * fit; }
    if (wide_test > 0) {
        double f2 = fmin(dw / (1920.0 * wide_test), dh / 1080.0);
        o->vw = 1920.0 * wide_test * f2; o->vh = 1080.0 * f2;
    }
    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;
}
