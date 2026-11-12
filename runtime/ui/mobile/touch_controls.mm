// (touch screens only: compiled empty elsewhere - the Makefile builds every runtime/ui/mobile/*.mm)
#include "../../platform.h"
#if PLAT_TOUCH
// On-screen controls for touch screens (phones, tablets), drawn as translucent blue bubbles with the game's own item
// icons on them (a white badge names the Switch button), a compass-rose stick and a larger A. Settings > Controls >
// Touch controls: Auto / Always / Off - Auto hides them while a controller is in use (it then plays as on the Switch)
// and a touch brings them back. SDL finger events come in from the window loop (runtime/hle/input.c), the state goes
// to the game with the keyboard's and the controller's (input.c fill_state), and the controls are drawn over the game
// by the overlay menu's ImGui frame (runtime/ui/menu.mm). Layout in units of the screen height, for landscape.
//
// Only what the game uses right now is on screen (Link's Awakening: A confirm / lift, B sword, X / Y items, R shield,
// L Pegasus Boots, Minus map, Plus items; ZL / ZR unused):
//   play        the stick, A, Plus / Minus, and B / R / L once Link has the sword / shield / boots, X / Y while an item
//               is on them (their icons: the live save's items word and X / Y slots)
//   title       (not in play: the opening, the title, the file select) a tap anywhere: skips the opening, presses L + R
//               on the title, on the file select puts the cursor on the file tapped (file_select_jump) and confirms,
//               then confirms its "load this file?"; the stick, A and B are there too
//   items page  (Plus) the stick, A, B "back", X / Y "equip", L / R, Plus / Minus; a tap on an item moves the game's
//               cursor straight there (one d-pad press, its move sent to that cell: inventory_items_jump)
//   map pages   (Minus, a dungeon's map) the stick, A, B "back", L / R, Plus / Minus
// Layout editor (Controls tab, "Edit touch layout"): over the game, drag a control to move it, tap it to select it; the
// bar sets the selected one's size, hides / shows it (hidden ones are drawn faint while editing), puts every control
// back, cancels or saves. Each control: settings touch_ctl_<name> = x, y (share of the screen), size, hidden.
// A quick tap still reaches the game (a button is held at least 80 ms). LARECOMP_TOUCH_SHOW_INPUT=1 (videos, tests):
// scripted input (LARECOMP_INPUT) lights the buttons and moves the stick as a thumb would.
#include <SDL3/SDL.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <string.h>

#include "imgui.h"
#include "../plastic.h"
#include "../../game_addresses.h"

extern "C" {
#include "../../hle.h"  // (LD8 / LD32, GUEST: the live save's items)
#include "../../settings.h"
bool input_gamepad_connected(void);  // runtime/hle/input.c
void input_script_state(uint64_t *buttons, int32_t *lx, int32_t *ly);
bool menu_input_captured(void);      // runtime/ui/menu.mm
void menu_toggle(void);
int game_in_play(void);              // runtime/hle/warp.c
int inventory_items_cursor(void);    // runtime/ui/inventory_overlay.mm
bool tracker_map_page_shown(void);   // runtime/ui/tracker_overlay.mm
bool chaos_dgnmap_shown(void);       // runtime/ui/chaos_overlay.mm
int texpatch_title_screen(void);     // runtime/nvn/texpatch.c
}
ImTextureID ui_game_icon(const char *name);  // runtime/ui/triforce_icons.mm

enum { BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_L = 6, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
       BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN };  // (input.c's bits)
#define BIT(b) (1ull << (b))
enum { C_STICK = 0, C_A, C_B, C_X, C_Y, C_L, C_R, C_ZL, C_ZR, C_PLUS, C_MINUS, C_MENU, C_START, C_EDIT, C_COUNT };
static_assert(C_COUNT == TOUCH_NCTL, "settings.h TOUCH_NCTL");
// (the ini names: settings.c's touch_control_name, this order)

// what the game shows: the controls each screen can have (play's are narrowed further by what Link has: wanted)
enum { X_PLAY = 0, X_TITLE, X_ITEMS, X_MAP, X_COUNT };
#define M(c) (1u << (c))
static const uint32_t kShown[X_COUNT] = {
    M(C_STICK) | M(C_A) | M(C_B) | M(C_X) | M(C_Y) | M(C_L) | M(C_R) | M(C_PLUS) | M(C_MINUS) | M(C_MENU) | M(C_EDIT),
    M(C_STICK) | M(C_A) | M(C_B) | M(C_MENU),  // (Start only where the player put it: a tap anywhere does its work)
    M(C_STICK) | M(C_A) | M(C_B) | M(C_X) | M(C_Y) | M(C_L) | M(C_R) | M(C_PLUS) | M(C_MINUS) | M(C_MENU),
    M(C_STICK) | M(C_A) | M(C_B) | M(C_L) | M(C_R) | M(C_PLUS) | M(C_MINUS) | M(C_MENU),
};
// (the editor: play's controls; the editor's own button while the setting shows it. ZL / ZR do nothing in the game:
// never shown, their slots kept for the settings file)
#define EDITABLE (kShown[X_PLAY] & (g_settings.touch_edit_button ? ~0u : ~M(C_EDIT)))

typedef struct {
    float x, y, r;       // centre and radius (round) / half height (pills: width 2.2 * r)
    uint64_t bits;       // buttons it holds
    const char *label;
    bool pill;
} Control;

static Control g_ctl[C_COUNT];
static float g_w, g_h, g_size;             // the screen and size setting the layout was made for (pixels)
static std::atomic<int> g_layout_dirty{1};
static std::mutex g_lock;                  // layout and fingers: the window loop's events, the render thread's drawing
static std::atomic<uint64_t> g_buttons{0};
static std::atomic<uint64_t> g_nav{0};     // presses made for a tap (the items cursor, the title's steps)
static std::atomic<int32_t> g_lx{0}, g_ly{0};
static std::atomic<int> g_ctx{X_PLAY};     // what the game shows, as of the last frame drawn
static std::atomic<uint32_t> g_want{0};    // the controls on screen now (context and what Link has)
static float g_stick_cx, g_stick_cy;       // where the stick's base is now (it follows the thumb's first touch)
static float g_knob_x, g_knob_y;
static bool g_menu_down, g_edit_down;
static double g_last_touch, g_last_pad;    // (SDL_GetTicks seconds) Auto: shown when a touch came after the pad's last use
#define MAX_FINGERS 10
static struct { SDL_FingerID id; int ctl; float dx, dy; } g_fingers[MAX_FINGERS];  // ctl -1 = free slot

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static double now_s(void) { return SDL_GetTicks() / 1000.0; }

// ---- the layout editor's state (render thread draws, window loop edits: under g_lock)
static bool g_editing;
static int g_sel = -1;                                // the selected control
static float g_epos[C_COUNT][2], g_escale[C_COUNT];   // the draft: positions (share of the screen) and sizes
static bool g_ehid[C_COUNT];
enum { E_SMALLER = 100, E_BIGGER, E_HIDE, E_RESET, E_CANCEL, E_DONE };
// the editor's buttons: - and + next to the selected control, Default / Cancel / Done in a row at the top
static const char *const kEditBar[] = {"Default", "Cancel", "Done"};
static float g_bar_x[3], g_bar_y, g_bar_w[3], g_bar_h;
static float g_tool_x[2], g_tool_y[2], g_tool_r;  // (- and +: none while nothing is selected)

static void defaults(float w, float h) {
    // (a tablet's screen is far taller than a thumb's reach: the layout a phone's size there, measured on an iPad)
    static const float device = plat_touch_tablet() ? 0.6f : 1.0f;
    float s = h * device * clampf(g_settings.touch_size, 0.6f, 1.6f);
    float br = 0.074f * s, gap = 0.135f * s;  // face buttons: a diamond, A the largest
    float fx = w - 0.27f * s, fy = h - 0.30f * s;
    g_ctl[C_A] = {fx + gap, fy + 0.01f * s, br * 1.22f, BIT(BTN_A), "A", false};
    g_ctl[C_B] = {fx - 0.01f * s, fy + gap, br, BIT(BTN_B), "B", false};
    g_ctl[C_X] = {fx, fy - gap, br, BIT(BTN_X), "X", false};
    g_ctl[C_Y] = {fx - gap, fy, br, BIT(BTN_Y), "Y", false};
    // L and R on the diamond's diagonal, as far out as each other (0.27), each angled to clear its two neighbours
    // evenly: R between X and A (up right), L between Y and B (down left). + straight above R, - out past L along
    // the bottom. ZL / ZR: never shown (wanted)
    float sr = 0.064f * s, d = 0.27f * s;
    const float ra = -46 * 3.14159265f / 180, la = 137 * 3.14159265f / 180;
    g_ctl[C_R] = {fx + cosf(ra) * d, fy + sinf(ra) * d, sr, BIT(BTN_R), "R", false};
    g_ctl[C_L] = {fx + cosf(la) * d, fy + sinf(la) * d, sr, BIT(BTN_L), "L", false};
    g_ctl[C_ZL] = {0.26f * s, 0.10f * s, sr * 0.8f, BIT(BTN_ZL), "ZL", false};
    g_ctl[C_ZR] = {w - 0.26f * s, 0.10f * s, sr * 0.8f, BIT(BTN_ZR), "ZR", false};
    float mr = 0.040f * s;
    g_ctl[C_PLUS] = {g_ctl[C_R].x, fy - 0.42f * s, mr, BIT(BTN_PLUS), "+", false};
    g_ctl[C_MINUS] = {fx - 0.43f * s, fy + 0.23f * s, mr, BIT(BTN_MINUS), "-", false};
    g_ctl[C_MENU] = {w * 0.5f, 0.06f * s, mr, 0, "", false};  // (the top: the menu's button, the layout editor's beside it)
    g_ctl[C_EDIT] = {w * 0.5f + 0.10f * s, 0.06f * s, mr * 0.8f, 0, "", false};
    g_ctl[C_START] = {w * 0.5f, h - 0.07f * s, 0.05f * s, BIT(BTN_L) | BIT(BTN_R) | BIT(BTN_A), "Start", true};
    g_ctl[C_STICK] = {0.25f * s, h - 0.27f * s, 0.14f * s, 0, "", false};
}

// the player's own places and sizes over the defaults (the editor's draft while editing)
static void layout(float w, float h) {
    g_w = w, g_h = h, g_size = g_settings.touch_size;
    defaults(w, h);
    for (int i = 0; i < C_COUNT; i++) {
        bool moved = g_editing ? g_epos[i][0] >= 0 : g_settings.touch_moved[i];
        float px = g_editing ? g_epos[i][0] : g_settings.touch_pos[i][0], py = g_editing ? g_epos[i][1] : g_settings.touch_pos[i][1];
        float sc = g_editing ? g_escale[i] : g_settings.touch_scale[i];
        if (moved) g_ctl[i].x = clampf(px, 0.01f, 0.99f) * w, g_ctl[i].y = clampf(py, 0.01f, 0.99f) * h;
        if (sc > 0.1f) g_ctl[i].r *= clampf(sc, 0.5f, 2.0f);
    }
    g_stick_cx = g_ctl[C_STICK].x, g_stick_cy = g_ctl[C_STICK].y;
    g_knob_x = g_stick_cx, g_knob_y = g_stick_cy;
    g_layout_dirty = 0;
}

static bool hidden(int i) { return g_editing ? g_ehid[i] : g_settings.touch_hidden[i]; }
static bool shown(int i) { return (g_want.load() >> i & 1) && !hidden(i); }

extern "C" bool touch_visible(void) {
    if (g_editing) return true;
    if (g_settings.touch_mode == 2) return false;
    if (g_settings.touch_mode == 1) return true;
    // Auto: while no controller is in use - a touch after its last button or stick brings them back (a phone has a
    // touch screen: not asked, SDL may list it only from its first touch on)
    return !input_gamepad_connected() || g_last_touch > g_last_pad;
}

// input.c: a controller was connected or used - Auto hides the controls until the next touch
extern "C" void touch_pad_used(void) {
    g_last_pad = now_s();
    if (g_last_touch >= g_last_pad) g_last_touch = 0;
}

static bool inside(const Control &c, float x, float y, float slack) {
    if (c.pill) return fabsf(x - c.x) <= c.r * 2.2f * slack && fabsf(y - c.y) <= c.r * 1.2f * slack;
    float dx = x - c.x, dy = y - c.y, r = c.r * slack;
    return dx * dx + dy * dy <= r * r;
}

// The control under (x, y): buttons get a generous slack (thumbs miss), the stick takes the lower left quarter
static int hit(float x, float y) {
    for (int i = C_A; i < C_COUNT; i++)
        if (shown(i) && inside(g_ctl[i], x, y, 1.3f)) return i;
    if (shown(C_STICK) && g_settings.touch_stick_fixed)  // (fixed: its own circle, a little beyond its rim)
        return inside(g_ctl[C_STICK], x, y, 1.6f) ? C_STICK : -1;
    if (shown(C_STICK) && x < g_w * 0.42f && y > g_h * 0.30f) return C_STICK;
    return -1;
}

// A quick tap still reaches the game: a button let go before 80 ms stays pressed until then (the game reads its input
// once a frame, every 33 ms at 30 fps - a tap shorter than that could fall between two reads)
static double g_held_until[C_COUNT];
static void recompute(void) {
    uint64_t b = 0;
    double t = now_s();
    for (int i = 0; i < MAX_FINGERS; i++) {
        int c = g_fingers[i].ctl;
        if (c > C_STICK && c < C_COUNT) b |= g_ctl[c].bits;
    }
    for (int c = C_A; c < C_COUNT; c++)
        if (t < g_held_until[c]) b |= g_ctl[c].bits;
    g_buttons = b;
}

static void stick_to(float x, float y) {
    float r = g_ctl[C_STICK].r, dx = x - g_stick_cx, dy = y - g_stick_cy, d = sqrtf(dx * dx + dy * dy);
    if (d > r) dx *= r / d, dy *= r / d;
    g_knob_x = g_stick_cx + dx, g_knob_y = g_stick_cy + dy;
    float nx = dx / r, ny = -dy / r, m = sqrtf(nx * nx + ny * ny);
    // Link has two speeds: he walks from about a third of the stick's tilt (11500 of 32767) and runs only at full tilt
    // (28000 still walks; measured with scripted stick holds). A thumb rarely holds a steady part-way tilt, so the
    // stick has two zones: a short one near the middle that walks (0.6: well inside the game's walking range) and the
    // rest, which runs. Once running it stays so down to 0.32, so a thumb at the edge doesn't flick between the two.
    static bool running;
    if (m < 0.12f) nx = ny = 0, running = false;  // dead zone
    else if (!g_settings.touch_dpad) {
        running = m >= (running ? 0.32f : 0.40f);
        float k = (running ? 1.0f : 0.6f) / m;
        nx *= k, ny *= k;
    } else {  // eight ways, full tilt (the game walks by the stick: its D-pad is a setting of its own)
        if (m < 0.25f) nx = ny = 0;
        else {
            float a = roundf(atan2f(ny, nx) / (3.14159265f / 4)) * (3.14159265f / 4);
            nx = cosf(a), ny = sinf(a);
        }
    }
    g_lx = (int32_t)lrintf(clampf(nx, -1, 1) * 32767), g_ly = (int32_t)lrintf(clampf(ny, -1, 1) * 32767);
}

// the stick taken / let go: the base comes to the thumb (kept on screen) - or stays where it is (Fixed)
static void stick_down(float x, float y) {
    float r = g_ctl[C_STICK].r;
    if (!g_settings.touch_stick_fixed)
        // (a whole radius plus a thumb's width clear of the edges: a base at the rim left no room to push the knob out
        // that way)
        g_stick_cx = clampf(x, r * 1.45f, g_w * 0.42f), g_stick_cy = clampf(y, g_h * 0.30f + r * 0.5f, g_h - r * 1.45f);
    stick_to(x, y);
}
static void stick_up(void) {
    g_lx = 0, g_ly = 0;
    g_knob_x = g_stick_cx = g_ctl[C_STICK].x, g_knob_y = g_stick_cy = g_ctl[C_STICK].y;
}

static void release_all(void) {
    for (int i = 0; i < MAX_FINGERS; i++) g_fingers[i].ctl = -1;
    g_buttons = 0, g_lx = 0, g_ly = 0;
    g_knob_x = g_stick_cx = g_ctl[C_STICK].x, g_knob_y = g_stick_cy = g_ctl[C_STICK].y;
    g_menu_down = false, g_edit_down = false;
}

// Everything let go (the overlay menu opened: it takes the touches, the lifts included)
extern "C" void touch_reset(void) {
    std::lock_guard<std::mutex> l(g_lock);
    release_all();
}

// ---- taps on the game's own screens
// A queue of presses made for a tap: each step holds its buttons for `on` seconds, then lets go for `off`
#define SEQ_MAX 16
static struct { uint64_t bits; float on, off; } g_seq[SEQ_MAX];
static int g_seq_n, g_seq_i;
static double g_seq_until;
static bool g_seq_pressing;
static void seq_start(void) { g_seq_n = g_seq_i = 0, g_seq_until = 0, g_seq_pressing = false; }
static void seq_add(uint64_t bits, float on, float off) {
    if (g_seq_n < SEQ_MAX) g_seq[g_seq_n++] = {bits, on, off};
}

// The game's menus are laid out on a 16:9 page, sized by the screen's height on wider screens (phones) and by its width
// on narrower ones (an iPad's 4:3: the page then sits in the middle, bands above and below - seen on a 1024x768
// window). page_at: a point as (across from the page's middle, down from its top), in shares of the page's height.
static void page_at(float x, float y, float *u, float *v) {
    float ph = fminf(g_h, g_w * 9.0f / 16.0f), top = (g_h - ph) * 0.5f;
    *u = (x - g_w * 0.5f) / ph, *v = (y - top) / ph;
}

// The Items page's 12 slots (4 x 3, cursor 0..11 row by row), measured on the phone (2400x1080): the first slot's centre
// 0.170 of the page's height right of its middle and 0.304 down, 0.144 apart both ways
static const float kGridX0 = 0.1704f, kGridY0 = 0.3037f, kGridD = 0.1444f;
static std::atomic<int> g_item_target{-1};  // the slot a tap asked for (the render thread steps the cursor there)
static int item_slot_at(float x, float y) {
    float u, v;
    page_at(x, y, &u, &v);
    int col = (int)floorf((u - (kGridX0 - kGridD * 0.5f)) / kGridD), row = (int)floorf((v - (kGridY0 - kGridD * 0.5f)) / kGridD);
    return col >= 0 && col < 4 && row >= 0 && row < 3 ? row * 4 + col : -1;
}

// The file select's four files (three diaries and the autosave), measured the same way: their centres down the
// page, each 0.19 of its height tall, 0.57 of it either side of the middle
static const float kFileY[4] = {0.195f, 0.400f, 0.605f, 0.856f};
static int file_at(float x, float y) {
    float u, v;
    page_at(x, y, &u, &v);
    if (fabsf(u) > 0.57f) return -1;
    for (int i = 0; i < 4; i++) if (fabsf(v - kFileY[i]) < 0.095f) return i;
    return -1;
}
// The file select's "load this file?" is up: a file was tapped and the file select is still shown
static bool g_file_picked;
extern "C" void file_select_jump(int file);  // runtime/ui/inventory_overlay.mm

// What the game shows before play, from what it draws: 1 the title, 2 the file select, 0 else (a movie, loading)
static int title_screen(void) {
    int s = texpatch_title_screen();
    if (s != 2) g_file_picked = false;
    return s;
}

static void title_tap(float x, float y) {
    int s = title_screen();
    if (s == 1) {  // the title: L + R
        seq_start();
        seq_add(BIT(BTN_L) | BIT(BTN_R), 0.12f, 0.10f);
        return;
    }
    if (s == 2 && !g_file_picked) {  // the file select: the file tapped (the game's cursor goes straight there on its
        int f = file_at(x, y);       // next frame), and in
        if (f < 0) return;
        file_select_jump(f);
        seq_start();
        seq_add(0, 0.10f, 0);  // (the game's frame that moves the cursor)
        seq_add(BIT(BTN_A), 0.10f, 0.10f);
        g_file_picked = true;
        return;
    }
    seq_start();  // a movie (skips it), "load this file?" (its first choice is the one to play): A
    seq_add(BIT(BTN_A), 0.10f, 0.05f);
}

// (each frame drawn) the presses for taps: the queue, then the items cursor to the tapped slot - a press (60 ms) whose
// move the game takes straight there (inventory_items_jump), again after 60 ms let go until the game's cursor is there
extern "C" void inventory_items_jump(int cell);  // runtime/ui/inventory_overlay.mm
static void nav_tick(void) {
    static double phase_until;
    static uint64_t phase_bits;
    double t = now_s();
    uint64_t nav = 0;
    if (g_seq_i < g_seq_n) {
        if (t >= g_seq_until) {
            if (!g_seq_pressing) g_seq_pressing = true, g_seq_until = t + g_seq[g_seq_i].on;
            else g_seq_pressing = false, g_seq_until = t + g_seq[g_seq_i].off, g_seq_i++;
        }
        if (g_seq_pressing && g_seq_i < g_seq_n) nav |= g_seq[g_seq_i].bits;
    }
    int want = g_item_target.load(), cur = want >= 0 ? inventory_items_cursor() : -1;
    if (want >= 0 && (cur < 0 || cur == want)) g_item_target = -1, phase_bits = 0;
    else if (want >= 0) {
        if (t >= phase_until) {
            if (phase_bits) phase_bits = 0;  // (let go between presses: the game moves on each new press)
            else {
                int dc = want % 4 - cur % 4, dr = want / 4 - cur / 4;
                phase_bits = dc > 0 ? BIT(BTN_RIGHT) : dc < 0 ? BIT(BTN_LEFT) : dr > 0 ? BIT(BTN_DOWN) : BIT(BTN_UP);
                inventory_items_jump(want);  // (the game's move then goes straight to it: one press)
            }
            phase_until = t + 0.06;
        }
        nav |= phase_bits;
    }
    g_nav = nav;
    recompute();  // (a quick tap's hold running out)
}

static bool handle(const SDL_Event *e, bool *toggle_menu);
extern "C" bool touch_handle_event(const SDL_Event *e) {
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP &&
        e->type != SDL_EVENT_FINGER_CANCELED)
        return false;
    bool toggle = false, r;
    {
        std::lock_guard<std::mutex> l(g_lock);
        r = handle(e, &toggle);
    }
    if (toggle) menu_toggle();  // (outside the lock: opening the menu lets every touch go, touch_reset)
    return r;
}

// The Touch tab's live preview (menu.mm): the stick / D-pad drawn over the game takes the thumb, to try each one
// before keeping it. Only the controls move: the game doesn't get the stick while the menu is open (input.c).
static bool g_preview_play;
extern "C" void touch_preview_play(bool on) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_preview_play && !on) release_all();  // (a thumb still on it as the preview closed)
    g_preview_play = on;
}
extern "C" bool touch_preview_event(const SDL_Event *e) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_preview_play || !g_w || g_editing) return false;
    float x = e->tfinger.x * g_w, y = e->tfinger.y * g_h;
    int slot = -1;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_fingers[i].ctl == C_STICK && g_fingers[i].id == e->tfinger.fingerID) slot = i;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (slot >= 0 || hit(x, y) != C_STICK) return false;  // (anything else: the menu's)
        for (int i = 0; i < MAX_FINGERS && slot < 0; i++) if (g_fingers[i].ctl < 0) slot = i;
        if (slot < 0) return false;
        g_fingers[slot].id = e->tfinger.fingerID, g_fingers[slot].ctl = C_STICK;
        stick_down(x, y);
        return true;
    }
    if (slot < 0) return false;
    if (e->type == SDL_EVENT_FINGER_MOTION) stick_to(x, y);
    else g_fingers[slot].ctl = -1, stick_up();
    return true;
}

// ---- the editor
extern "C" bool touch_edit_available(void) { return g_w > 0 && game_in_play(); }
static void edit_begin(void) {  // (g_lock held)
    for (int i = 0; i < C_COUNT; i++) {
        g_epos[i][0] = g_settings.touch_moved[i] ? g_settings.touch_pos[i][0] : -1;
        g_epos[i][1] = g_settings.touch_pos[i][1];
        g_escale[i] = g_settings.touch_scale[i] > 0.1f ? g_settings.touch_scale[i] : 1;
        g_ehid[i] = g_settings.touch_hidden[i];
    }
    g_editing = true, g_sel = -1;
    release_all();
    g_layout_dirty = 1;
}
extern "C" void touch_edit_begin(void) {
    std::lock_guard<std::mutex> l(g_lock);
    edit_begin();
}
static void edit_end(bool save) {
    if (save) {
        for (int i = 0; i < C_COUNT; i++) {
            g_settings.touch_moved[i] = g_epos[i][0] >= 0;
            g_settings.touch_pos[i][0] = g_epos[i][0] >= 0 ? g_epos[i][0] : 0;
            g_settings.touch_pos[i][1] = g_epos[i][0] >= 0 ? g_epos[i][1] : 0;
            g_settings.touch_scale[i] = g_escale[i];
            g_settings.touch_hidden[i] = g_ehid[i];
        }
        settings_save();
    }
    g_editing = false, g_sel = -1;
    release_all();
    g_layout_dirty = 1;
}
extern "C" void touch_layout_reset(void) {
    std::lock_guard<std::mutex> l(g_lock);
    for (int i = 0; i < C_COUNT; i++) g_settings.touch_moved[i] = false, g_settings.touch_scale[i] = 1, g_settings.touch_hidden[i] = false;
    settings_save();
    g_layout_dirty = 1;
}
// the Android back button while editing: cancel (menu.mm)
extern "C" bool touch_edit_back(void) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_editing) return false;
    edit_end(false);
    return true;
}

static int bar_hit(float x, float y) {
    if (g_sel >= 0)
        for (int i = 0; i < 2; i++) {
            float dx = x - g_tool_x[i], dy = y - g_tool_y[i], r = g_tool_r * 1.2f;
            if (dx * dx + dy * dy <= r * r) return E_SMALLER + i;
        }
    for (int i = 0; i < 3; i++)
        if (x >= g_bar_x[i] && x <= g_bar_x[i] + g_bar_w[i] && y >= g_bar_y && y <= g_bar_y + g_bar_h) return E_RESET + i;
    return -1;
}

static bool edit_event(const SDL_Event *e, float x, float y, int slot) {
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        int b = bar_hit(x, y);
        if (b >= 0) {
            int s = g_sel;
            if (b == E_SMALLER && s >= 0) g_escale[s] = clampf(g_escale[s] - 0.1f, 0.5f, 2.0f);
            if (b == E_BIGGER && s >= 0) g_escale[s] = clampf(g_escale[s] + 0.1f, 0.5f, 2.0f);
            if (b == E_RESET) for (int i = 0; i < C_COUNT; i++) g_epos[i][0] = -1, g_escale[i] = 1, g_ehid[i] = false;
            if (b == E_CANCEL || b == E_DONE) { edit_end(b == E_DONE); return true; }
            g_layout_dirty = 1;
            return true;
        }
        int c = -1;  // any control of the play screen, the hidden ones too (to show them again)
        for (int i = 0; i < C_COUNT && c < 0; i++)
            if ((EDITABLE >> i & 1) && inside(g_ctl[i], x, y, i == C_STICK ? 1.0f : 1.2f)) c = i;
        g_sel = c;
        if (c < 0) return true;
        for (int i = 0; i < MAX_FINGERS && slot < 0; i++) if (g_fingers[i].ctl < 0) slot = i;
        if (slot < 0) return true;
        g_fingers[slot].id = e->tfinger.fingerID, g_fingers[slot].ctl = 200 + c;
        g_fingers[slot].dx = g_ctl[c].x - x, g_fingers[slot].dy = g_ctl[c].y - y;
        return true;
    }
    if (slot < 0 || g_fingers[slot].ctl < 200) return true;
    int c = g_fingers[slot].ctl - 200;
    if (e->type == SDL_EVENT_FINGER_MOTION) {
        g_epos[c][0] = clampf((x + g_fingers[slot].dx) / g_w, 0.02f, 0.98f);
        g_epos[c][1] = clampf((y + g_fingers[slot].dy) / g_h, 0.03f, 0.97f);
        g_layout_dirty = 1;
    } else {
        g_fingers[slot].ctl = -1;
    }
    return true;
}

static bool handle(const SDL_Event *e, bool *toggle_menu) {
    static bool init;
    if (!init) { init = true; for (int i = 0; i < MAX_FINGERS; i++) g_fingers[i].ctl = -1; }
    if (e->type == SDL_EVENT_FINGER_DOWN) g_last_touch = now_s();
    if (!g_w) return false;
    float x = e->tfinger.x * g_w, y = e->tfinger.y * g_h;
    int slot = -1;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_fingers[i].ctl >= 0 && g_fingers[i].id == e->tfinger.fingerID) slot = i;
    if (g_editing) return edit_event(e, x, y, slot);
    if (menu_input_captured() || !touch_visible()) { release_all(); return false; }  // (the menu takes touches)
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        int c = hit(x, y);
        if (c < 0) {  // a tap on the game's own screen
            int ctx = g_ctx.load();
            if (ctx == X_TITLE) title_tap(x, y);
            else if (ctx == X_ITEMS) { int s = item_slot_at(x, y); if (s >= 0) g_item_target = s; }
            return true;
        }
        if (slot < 0) for (int i = 0; i < MAX_FINGERS && slot < 0; i++) if (g_fingers[i].ctl < 0) slot = i;
        if (slot < 0) return true;
        g_fingers[slot].id = e->tfinger.fingerID, g_fingers[slot].ctl = c;
        if (c > C_STICK) g_held_until[c] = now_s() + 0.08;
        if (g_ctx.load() == X_TITLE && c == C_B) g_file_picked = false;  // (B backs out of "load this file?")
        if (c == C_STICK) stick_down(x, y);
        else if (c == C_MENU) {
            g_menu_down = true;
        } else if (c == C_EDIT) {
            g_edit_down = true;
        }
        recompute();
        return true;
    }
    if (slot < 0) return true;
    int c = g_fingers[slot].ctl;
    if (e->type == SDL_EVENT_FINGER_MOTION) {
        if (c == C_STICK) stick_to(x, y);
        else if (c >= C_A && c <= C_Y) {  // sliding across the face buttons presses the one under the thumb
            int n = hit(x, y);
            if (n >= C_A && n <= C_Y && n != c) g_fingers[slot].ctl = n, g_held_until[n] = now_s() + 0.08;
        }
        recompute();
        return true;
    }
    // up / cancelled
    g_fingers[slot].ctl = -1;
    if (c == C_STICK) stick_up();
    if (c == C_MENU && g_menu_down) {
        g_menu_down = false;
        if (e->type == SDL_EVENT_FINGER_UP) *toggle_menu = true;
    }
    if (c == C_EDIT && g_edit_down) {  // (let go on it: the layout editor, over the game)
        g_edit_down = false;
        if (e->type == SDL_EVENT_FINGER_UP && game_in_play()) { edit_begin(); return true; }
    }
    recompute();
    return true;
}

extern "C" uint64_t touch_buttons(void) { return g_editing ? 0 : g_buttons | g_nav; }
extern "C" void touch_stick(int32_t *lx, int32_t *ly) { *lx = g_editing ? 0 : g_lx.load(), *ly = g_editing ? 0 : g_ly.load(); }

// ---- what the game shows, what Link has
static int context(void) {
    if (!game_in_play()) return X_TITLE;
    if (inventory_items_cursor() >= 0) return X_ITEMS;
    if (tracker_map_page_shown() || chaos_dgnmap_shown()) return X_MAP;
    return X_PLAY;
}

// the icons: the items word's bits / the X and Y slots' numbers (the game's own UI textures, Game.arc)
static const char *slot_icon(int slot) {
    static const char *const names[16] = {
        "EqSword_01^q", "EqShield_00^q", "ItemBomb_00^w", "ItemBow_00^w", "ItemFookShot_00^w", "ItemBoomerang_00^w",
        "ItemMagicRod_00^w", "ItemShovel_00^w", "ItemMagicPowder_00^w", "ItemRocsFeather_00^w", "EqPowerBracelet_00^q",
        "EqPegasusBoots_00^q", "ItemOcarina_03^w", "ItemBottle_00^w", "ItemBottle_00^w", "ItemBottle_00^w"};
    return slot >= 0 && slot < 16 ? names[slot] : NULL;
}

static uint32_t g_items_word;
static int g_slot_x, g_slot_y;  // (0 / 1, the sword's and shield's slots, never sit on X / Y: an empty one reads 0)
static uint32_t wanted(int ctx) {
    uint32_t m = kShown[ctx] & (g_settings.touch_edit_button ? ~0u : ~M(C_EDIT));
    if (ctx == X_PLAY || ctx == X_ITEMS) {  // (the items page too: X / Y show what's just been equipped)
        g_items_word = LD32(GA(LIVE_SAVE) + 0x5568);
        g_slot_y = LD8(GA(LIVE_SAVE) + 0x5562), g_slot_x = LD8(GA(LIVE_SAVE) + 0x5563);
    }
    if (ctx == X_PLAY && g_settings.touch_all) return m;  // (every button, whatever Link has)
    if (ctx == X_PLAY) {
        if (!(g_items_word & 1)) m &= ~M(C_B);           // the sword
        if (!(g_items_word & 2)) m &= ~M(C_R);           // the shield
        if (!(g_items_word >> 11 & 1)) m &= ~M(C_L);     // the Pegasus Boots
        if (g_slot_x <= 1 || g_slot_x > 15) m &= ~M(C_X);
        if (g_slot_y <= 1 || g_slot_y > 15) m &= ~M(C_Y);
    }
    return m;
}

static const char *icon_of(int ctx, int i) {
    if (ctx == X_PLAY || ctx == X_ITEMS) {
        if (i == C_B && ctx == X_PLAY) return slot_icon(0);
        if (i == C_R && ctx == X_PLAY) return slot_icon(1);
        if (i == C_L && ctx == X_PLAY) return slot_icon(11);
        if (i == C_X && g_slot_x > 1) return slot_icon(g_slot_x);
        if (i == C_Y && g_slot_y > 1) return slot_icon(g_slot_y);
    }
    return NULL;
}

static const char *hint(int ctx, int i) {
    if (ctx == X_ITEMS) return i == C_X || i == C_Y ? "Equip" : i == C_B ? "Back" : NULL;
    if (ctx == X_MAP) return i == C_B ? "Back" : NULL;
    return NULL;
}

// ---- drawing: bubbles
static ImU32 col(int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)clampf(a * 255, 0, 255)); }

// a glossy bubble: a translucent body, a lighter top, a bright rim
static void bubble(ImDrawList *dl, ImVec2 c, float r, bool on, bool teal, float a, float th, bool sel) {
    a = fmaxf(a, 0.55f);  // (the opacity setting fades them, never to washed out)
    int br = teal ? 30 : 38, bg = teal ? 170 : 128, bb = teal ? 150 : 200;
    if (on) br += 70, bg += 60, bb += 40;
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.06f), r * 1.02f, col(5, 30, 60, 0.35f * a), 48);       // (shadow)
    dl->AddCircleFilled(c, r, col(br / 2, bg / 2, bb / 2, 0.80f * a), 48);                            // dark rim
    dl->AddCircleFilled(c, r * 0.90f, col(br, bg, bb, (on ? 0.95f : 0.72f) * a), 48);                  // body
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.22f), r * 0.62f, col(br + 30, bg + 40, bb + 30, 0.35f * a), 40);  // glow
    dl->AddCircleFilled(ImVec2(c.x - r * 0.30f, c.y - r * 0.40f), r * 0.24f, col(255, 255, 255, 0.28f * a), 24);  // gloss
    dl->AddCircle(c, r * 0.90f, sel ? col(236, 194, 84, 1) : col(185, 232, 255, 0.95f * a), 48, (sel ? 5.0f : 2.8f) * th);
}

// The inventory's own item slot, from the game's UI textures (Game.arc): the cork disc, and pressed, lit as the slot
// of an equipped item - the green ring round a pale disc. false: the textures aren't there (the bubble instead).
static bool slot_disc(ImDrawList *dl, ImVec2 c, float r, bool on, float a, float th, bool sel) {
    ImTextureID dark = ui_game_icon("ItemBG_00^q"), green = ui_game_icon("ItemSetBG_00^q"), pale = ui_game_icon("ItemBG_01^q");
    if (!dark || !green || !pale) return false;
    auto disc = [&](ImTextureID t, float rad, float span, ImU32 tint) {  // (span: the image's half width per disc radius)
        float s = rad * span;
        dl->AddImage(t, ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), ImVec2(0, 0), ImVec2(1, 1), tint);
    };
    a = fminf(1, 0.55f + 0.5f * a);  // (mostly solid: 0.85 at the opacity setting's default)
    if (on) {
        disc(green, r, 96.0f / 66, col(255, 255, 255, fminf(1, a + 0.15f)));
        disc(pale, r * 0.90f, 72.0f / 59, col(235, 192, 130, fminf(1, a + 0.15f)));
    } else disc(dark, r, 96.0f / 66, col(249, 205, 206, a));  // (the tints: the shades the game draws them in)
    if (sel) dl->AddCircle(c, r * 1.02f, col(236, 194, 84, 1), 48, 5.0f * th);
    return true;
}

// the Switch button's letter on a small white badge, upper left of the bubble
static void badge(ImDrawList *dl, ImFont *font, ImVec2 c, float r, const char *lab, float a) {
    float br = fmaxf(r * 0.34f, 11.0f);
    ImVec2 p(c.x - r * 0.70f, c.y - r * 0.70f);
    dl->AddCircleFilled(ImVec2(p.x + 1.5f, p.y + 2.0f), br, col(0, 0, 0, 0.30f * a), 24);
    dl->AddCircleFilled(p, br, col(250, 252, 255, 0.95f * fmaxf(a, 0.7f)), 24);
    float fs = br * (strlen(lab) > 1 ? 1.05f : 1.45f);
    ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0, lab);
    dl->AddText(font, fs, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), col(30, 60, 105, 1), lab);
    dl->AddText(font, fs, ImVec2(p.x - ts.x * 0.5f + fs * 0.05f, p.y - ts.y * 0.5f), col(30, 60, 105, 1), lab);
}

static void icon(ImDrawList *dl, ImTextureID id, ImVec2 c, float r, float a) {
    float s = r * 1.5f;
    dl->AddImage(id, ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), ImVec2(c.x + s * 0.5f, c.y + s * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
                 col(255, 255, 255, fmaxf(a, 0.75f)));
}

// the stick: a compass rose under a glossy knob
static void compass(ImDrawList *dl, ImVec2 c, float r, ImVec2 knob, float a, float th, bool sel) {
    a = fmaxf(a, 0.55f);
    dl->AddCircleFilled(c, r, col(30, 100, 165, 0.45f * a), 64);
    dl->AddCircle(c, r, sel ? col(236, 194, 84, 1) : col(190, 235, 255, 0.80f * a), 64, (sel ? 5.0f : 2.6f) * th);
    dl->AddCircle(c, r * 0.80f, col(190, 235, 255, 0.35f * a), 64, 1.6f * th);
    for (int k = 0; k < 8; k++) {  // eight points: the four long ones bright
        float ang = k * 3.14159265f / 4, len = k % 2 ? 0.62f : 0.92f, wd = k % 2 ? 0.10f : 0.14f;
        ImVec2 tip(c.x + cosf(ang) * r * len, c.y + sinf(ang) * r * len);
        ImVec2 s1(c.x + cosf(ang + 1.5708f) * r * wd, c.y + sinf(ang + 1.5708f) * r * wd);
        ImVec2 s2(c.x - cosf(ang + 1.5708f) * r * wd, c.y - sinf(ang + 1.5708f) * r * wd);
        dl->AddTriangleFilled(tip, s1, s2, col(205, 240, 255, (k % 2 ? 0.30f : 0.45f) * a));
    }
    float kr = r * 0.42f;
    dl->AddCircleFilled(ImVec2(knob.x, knob.y + kr * 0.12f), kr, col(0, 0, 0, 0.25f * a), 40);
    dl->AddCircleFilled(knob, kr, col(70, 165, 225, 0.85f * a), 40);
    dl->AddCircleFilled(ImVec2(knob.x, knob.y - kr * 0.32f), kr * 0.6f, col(255, 255, 255, 0.14f * a), 32);
    dl->AddCircle(knob, kr, col(220, 245, 255, 0.95f * a), 40, 2.4f * th);
    float d = kr * 0.32f;  // a small diamond on it
    dl->AddQuadFilled(ImVec2(knob.x, knob.y - d), ImVec2(knob.x + d * 0.7f, knob.y), ImVec2(knob.x, knob.y + d),
                      ImVec2(knob.x - d * 0.7f, knob.y), col(230, 248, 255, 0.85f * a));
}

// the D-pad: the stick's disc with a glossy cross on it, in the bubbles' colours, the arms held down lit
static void dpad(ImDrawList *dl, ImVec2 c, float r, int32_t lx, int32_t ly, float a, float th, bool sel) {
    a = fmaxf(a, 0.55f);
    dl->AddCircleFilled(c, r, col(30, 100, 165, 0.45f * a), 64);
    dl->AddCircle(c, r, sel ? col(236, 194, 84, 1) : col(190, 235, 255, 0.80f * a), 64, (sel ? 5.0f : 2.6f) * th);
    dl->AddCircle(c, r * 0.80f, col(190, 235, 255, 0.35f * a), 64, 1.6f * th);
    float len = r * 0.84f, w = r * 0.29f, in = r * 0.05f, rnd = r * 0.10f;
    ImU32 shadow = col(5, 30, 60, 0.35f * a), rim = col(19, 64, 100, 0.80f * a), body = col(38, 128, 200, 0.72f * a);
    ImU32 lit = col(108, 188, 240, 0.95f * a), gloss = col(255, 255, 255, 0.22f * a), edge = col(185, 232, 255, 0.95f * a);
    float dy = r * 0.05f;
    dl->AddRectFilled(ImVec2(c.x - w, c.y - len + dy), ImVec2(c.x + w, c.y + len + dy), shadow, rnd);
    dl->AddRectFilled(ImVec2(c.x - len, c.y - w + dy), ImVec2(c.x + len, c.y + w + dy), shadow, rnd);
    dl->AddRectFilled(ImVec2(c.x - w, c.y - len), ImVec2(c.x + w, c.y + len), rim, rnd);
    dl->AddRectFilled(ImVec2(c.x - len, c.y - w), ImVec2(c.x + len, c.y + w), rim, rnd);
    float w2 = w - in, l2 = len - in;
    const bool up = ly > 8000, down = ly < -8000, left = lx < -8000, right = lx > 8000;
    dl->AddRectFilled(ImVec2(c.x - w2, c.y - l2), ImVec2(c.x + w2, c.y + l2), body, rnd * 0.7f);
    dl->AddRectFilled(ImVec2(c.x - l2, c.y - w2), ImVec2(c.x + l2, c.y + w2), body, rnd * 0.7f);
    if (up) dl->AddRectFilled(ImVec2(c.x - w2, c.y - l2), ImVec2(c.x + w2, c.y - w2), lit, rnd * 0.7f);
    if (down) dl->AddRectFilled(ImVec2(c.x - w2, c.y + w2), ImVec2(c.x + w2, c.y + l2), lit, rnd * 0.7f);
    if (left) dl->AddRectFilled(ImVec2(c.x - l2, c.y - w2), ImVec2(c.x - w2, c.y + w2), lit, rnd * 0.7f);
    if (right) dl->AddRectFilled(ImVec2(c.x + w2, c.y - w2), ImVec2(c.x + l2, c.y + w2), lit, rnd * 0.7f);
    dl->AddRectFilled(ImVec2(c.x - l2 + in, c.y - w2 + in * 0.4f), ImVec2(c.x + l2 - in, c.y - w2 + in * 2.0f), gloss, in);  // (the sheen along the top)
    dl->AddRectFilled(ImVec2(c.x - w2 + in * 0.4f, c.y - l2 + in), ImVec2(c.x - w2 + in * 2.0f, c.y - w - in), gloss, in);
    ImVec2 o[12] = {{c.x - w2, c.y - l2}, {c.x + w2, c.y - l2}, {c.x + w2, c.y - w2}, {c.x + l2, c.y - w2}, {c.x + l2, c.y + w2}, {c.x + w2, c.y + w2},
                    {c.x + w2, c.y + l2}, {c.x - w2, c.y + l2}, {c.x - w2, c.y + w2}, {c.x - l2, c.y + w2}, {c.x - l2, c.y - w2}, {c.x - w2, c.y - w2}};
    dl->AddPolyline(o, 12, edge, ImDrawFlags_Closed, 2.2f * th);
    float kr = r * 0.17f;  // (the middle: the stick's knob, small)
    dl->AddCircleFilled(c, kr, col(70, 165, 225, 0.85f * a), 32);
    dl->AddCircle(c, kr, col(220, 245, 255, 0.95f * a), 32, 1.8f * th);
    float t = len * 0.90f, h = len * 0.62f, s = r * 0.14f;
    ImU32 ar = col(245, 252, 255, fmaxf(a, 0.85f));
    dl->AddTriangleFilled(ImVec2(c.x, c.y - t), ImVec2(c.x + s, c.y - h), ImVec2(c.x - s, c.y - h), ar);
    dl->AddTriangleFilled(ImVec2(c.x, c.y + t), ImVec2(c.x - s, c.y + h), ImVec2(c.x + s, c.y + h), ar);
    dl->AddTriangleFilled(ImVec2(c.x - t, c.y), ImVec2(c.x - h, c.y - s), ImVec2(c.x - h, c.y + s), ar);
    dl->AddTriangleFilled(ImVec2(c.x + t, c.y), ImVec2(c.x + h, c.y + s), ImVec2(c.x + h, c.y - s), ar);
}

// The stick and the D-pad to go with the slot discs: the cork disc for a base. The stick: the compass points engraved
// in brown, the knob a small slot disc, lit while it's held. The D-pad: a cross in the lit slot's tan, edged in brown,
// the arms held down green. false: the textures aren't there (the blue ones instead).
static bool compass_disc(ImDrawList *dl, ImVec2 c, float r, ImVec2 knob, float a, float th, bool sel) {
    if (!slot_disc(dl, c, r, false, a, th, sel)) return false;
    float ea = fminf(1, 0.55f + 0.5f * a);
    for (int k = 0; k < 8; k++) {
        float ang = k * 3.14159265f / 4, len = k % 2 ? 0.62f : 0.92f, wd = k % 2 ? 0.10f : 0.14f;
        ImVec2 tip(c.x + cosf(ang) * r * len, c.y + sinf(ang) * r * len);
        ImVec2 s1(c.x + cosf(ang + 1.5708f) * r * wd, c.y + sinf(ang + 1.5708f) * r * wd);
        ImVec2 s2(c.x - cosf(ang + 1.5708f) * r * wd, c.y - sinf(ang + 1.5708f) * r * wd);
        dl->AddTriangleFilled(tip, s1, s2, col(74, 52, 26, (k % 2 ? 0.45f : 0.70f) * ea));
    }
    bool held = hypotf(knob.x - c.x, knob.y - c.y) > r * 0.04f;
    slot_disc(dl, knob, r * 0.42f, held, 1.0f, th, false);
    return true;
}

static bool dpad_disc(ImDrawList *dl, ImVec2 c, float r, int32_t lx, int32_t ly, float a, float th, bool sel) {
    if (!slot_disc(dl, c, r, false, a, th, sel)) return false;
    float ea = fminf(1, 0.55f + 0.5f * a), len = r * 0.84f, w = r * 0.29f, rnd = r * 0.08f;
    ImU32 tan = col(235, 192, 130, ea), lit = col(140, 220, 70, ea), ink = col(74, 52, 26, ea);
    dl->AddRectFilled(ImVec2(c.x - w, c.y - len), ImVec2(c.x + w, c.y + len), tan, rnd);
    dl->AddRectFilled(ImVec2(c.x - len, c.y - w), ImVec2(c.x + len, c.y + w), tan, rnd);
    float in = 2.5f * th;  // (the held arm inside the edge)
    if (ly > 8000) dl->AddRectFilled(ImVec2(c.x - w + in, c.y - len + in), ImVec2(c.x + w - in, c.y - w), lit, rnd);
    if (ly < -8000) dl->AddRectFilled(ImVec2(c.x - w + in, c.y + w), ImVec2(c.x + w - in, c.y + len - in), lit, rnd);
    if (lx < -8000) dl->AddRectFilled(ImVec2(c.x - len + in, c.y - w + in), ImVec2(c.x - w, c.y + w - in), lit, rnd);
    if (lx > 8000) dl->AddRectFilled(ImVec2(c.x + w, c.y - w + in), ImVec2(c.x + len - in, c.y + w - in), lit, rnd);
    ImVec2 o[12] = {{c.x - w, c.y - len}, {c.x + w, c.y - len}, {c.x + w, c.y - w}, {c.x + len, c.y - w}, {c.x + len, c.y + w}, {c.x + w, c.y + w},
                    {c.x + w, c.y + len}, {c.x - w, c.y + len}, {c.x - w, c.y + w}, {c.x - len, c.y + w}, {c.x - len, c.y - w}, {c.x - w, c.y - w}};
    dl->AddPolyline(o, 12, ink, ImDrawFlags_Closed, 3.5f * th);
    float t = len * 0.90f, h = len * 0.62f, s = r * 0.14f;
    dl->AddTriangleFilled(ImVec2(c.x, c.y - t), ImVec2(c.x + s, c.y - h), ImVec2(c.x - s, c.y - h), ink);
    dl->AddTriangleFilled(ImVec2(c.x, c.y + t), ImVec2(c.x - s, c.y + h), ImVec2(c.x + s, c.y + h), ink);
    dl->AddTriangleFilled(ImVec2(c.x - t, c.y), ImVec2(c.x - h, c.y - s), ImVec2(c.x - h, c.y + s), ink);
    dl->AddTriangleFilled(ImVec2(c.x + t, c.y), ImVec2(c.x + h, c.y + s), ImVec2(c.x + h, c.y - s), ink);
    return true;
}

static void label_text(ImDrawList *dl, ImFont *font, float fs, ImVec2 c, const char *t, ImU32 colr, bool shadow, bool bold = false) {
    ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0, t);
    ImVec2 p(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f);
    if (shadow) dl->AddText(font, fs, ImVec2(p.x + 2, p.y + 2), col(0, 0, 0, 0.55f), t);
    dl->AddText(font, fs, p, colr, t);
    if (bold) dl->AddText(font, fs, ImVec2(p.x + fs * 0.04f, p.y), colr, t);  // (a heavier stroke: the letters on bubbles)
}

// Drawn over the game (the overlay menu's frame, before its windows)
static std::atomic<bool> g_title_box{false};
extern "C" bool touch_title_box(void) { return g_title_box.load(); }  // (menu.mm: its panel needs the pieces this frame)

extern "C" void touch_draw(ImDrawList *dl, float w, float h, ImFont *font, ImFont *menu_font) {
    std::lock_guard<std::mutex> l(g_lock);
    g_title_box = false;
    if (w != g_w || h != g_h || g_size != g_settings.touch_size || g_layout_dirty.load()) {  // (a new screen, a setting)
        if (!g_editing) release_all();
        layout(w, h);
    }
    int ctx = g_editing ? X_PLAY : context();
    if (ctx != g_ctx.load()) {
        g_ctx = ctx;
        release_all();
    }
    g_want = g_editing ? EDITABLE : wanted(ctx);
    if (g_editing) wanted(X_PLAY);  // (the icons)
    nav_tick();
    float a = clampf(g_settings.touch_opacity, 0.15f, 1.0f);
    uint64_t b = g_buttons | g_nav;
    float knob_x = g_knob_x, knob_y = g_knob_y;
    static int show_input = -1;  // LARECOMP_TOUCH_SHOW_INPUT=1: scripted input shown on the controls
    if (show_input < 0) show_input = getenv("LARECOMP_TOUCH_SHOW_INPUT") ? atoi(getenv("LARECOMP_TOUCH_SHOW_INPUT")) != 0 : 0;
    if (show_input && !g_editing) {
        uint64_t sb;
        int32_t slx, sly;
        input_script_state(&sb, &slx, &sly);
        b |= sb;
        if (slx || sly) {
            float r = g_ctl[C_STICK].r;
            knob_x = g_stick_cx + slx / 32767.0f * r * 0.85f, knob_y = g_stick_cy - sly / 32767.0f * r * 0.85f;
        }
    }
    float th = h / 1080.0f;
    float tt = th * 1.35f;  // (text: a third larger on a phone, as the menu's kUi)
    if (g_editing) dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 110));
    uint32_t want = g_want.load();
    for (int i = C_A; i < C_COUNT; i++) {
        if (!(want >> i & 1) || (!g_editing && hidden(i))) continue;
        float fa = a * (g_editing && hidden(i) ? 0.35f : 1.0f);  // (hidden: faint while editing)
        const Control &c = g_ctl[i];
        bool on = !g_editing && (((c.bits & b) && (c.bits & b) == c.bits) || (i == C_MENU && g_menu_down) || (i == C_EDIT && g_edit_down));
        bool sel = g_editing && i == g_sel;
        ImVec2 p(c.x, c.y);
        float r = on ? c.r * 0.95f : c.r;
        if (c.pill) {  // (Start: a teal pill)
            ImVec2 p0(c.x - c.r * 2.2f, c.y - c.r * 1.2f), p1(c.x + c.r * 2.2f, c.y + c.r * 1.2f);
            dl->AddRectFilled(p0, p1, col(40 + (on ? 60 : 0), 178, 160, (on ? 0.85f : 0.6f) * fa), c.r * 1.2f);
            dl->AddRect(p0, p1, sel ? col(236, 194, 84, 1) : col(200, 255, 240, 0.9f * fa), c.r * 1.2f, 0, 2.6f * th);
            label_text(dl, font, c.r * 0.85f, p, "Start", col(255, 255, 255, fmaxf(fa, 0.8f)), true);
            continue;
        }
        bool disc = slot_disc(dl, p, r, on, fa, th, sel);
        if (!disc) bubble(dl, p, r, on, i == C_A, fa, th, sel);
        ImU32 ink = disc ? col(74, 52, 26, fmaxf(fa, 0.9f)) : col(255, 255, 255, fmaxf(fa, 0.85f));  // (on a disc: the game's brown)
        if (i == C_EDIT) {  // a pencil, tip down to the left
            float u = r * 0.55f, d = 0.7071f, wd = u * 0.24f;
            ImVec2 a(c.x - u * 0.55f, c.y + u * 0.55f), z(c.x + u * 0.62f, c.y - u * 0.62f);  // (the tip's base, the top)
            ImVec2 n(d * wd, d * wd);  // (across the body)
            dl->AddQuadFilled(ImVec2(a.x - n.x, a.y - n.y), ImVec2(z.x - n.x, z.y - n.y), ImVec2(z.x + n.x, z.y + n.y), ImVec2(a.x + n.x, a.y + n.y), ink);
            dl->AddTriangleFilled(ImVec2(a.x - n.x, a.y - n.y), ImVec2(a.x + n.x, a.y + n.y), ImVec2(a.x - u * 0.35f, a.y + u * 0.35f), ink);
            continue;
        }
        if (i == C_MENU) {  // three bars
            float u = r * 0.42f;
            for (int k = -1; k <= 1; k++)
                dl->AddLine(ImVec2(c.x - u, c.y + k * u * 0.6f), ImVec2(c.x + u, c.y + k * u * 0.6f), ink, 3 * th);
            continue;
        }
        const char *in = g_editing && i != C_X && i != C_Y ? icon_of(X_PLAY, i) : icon_of(ctx, i);
        ImTextureID tex = in ? ui_game_icon(in) : (ImTextureID)0;
        if (tex) {
            icon(dl, tex, p, r, fa);
            badge(dl, font, p, r, c.label, fa);
        } else {  // no icon: the letter in the middle
            bool pm = c.label[0] == '+' || c.label[0] == '-';
            label_text(dl, font, r * (pm ? 1.4f : i == C_A ? 1.15f : 0.95f), p, c.label, ink, !disc, true);
        }
        const char *hn = g_editing ? NULL : hint(ctx, i);
        if (hn && *hn) {  // what it does here: a small brown tab on the button's bottom rim (the top one when there's
                          // no room), in the menu's font (Baloo 2)
            float hs = fmaxf(r * 0.36f, 20 * tt);
            ImVec2 hz = menu_font->CalcTextSizeA(hs, 1e9f, 0, hn);
            float ph = hs * 1.05f, pw = hz.x + hs * 0.8f, ty = c.y + r - ph * 0.15f;
            if (ty + ph * 0.5f > h) ty = c.y - r + ph * 0.15f;
            ImVec2 p0(c.x - pw * 0.5f, ty - ph * 0.5f), p1(c.x + pw * 0.5f, ty + ph * 0.5f);
            dl->AddRectFilled(ImVec2(p0.x + 2 * th, p0.y + 4 * th), ImVec2(p1.x + 2 * th, p1.y + 4 * th), col(0, 0, 0, 0.35f), ph * 0.5f);
            dl->AddRectFilled(p0, p1, col(74, 52, 26, 0.92f), ph * 0.5f);
            dl->AddRect(p0, p1, col(235, 192, 130, 1), ph * 0.5f, 0, 2.5f * th);
            dl->AddText(menu_font, hs, ImVec2(c.x - hz.x * 0.5f, ty - hz.y * 0.5f - hs * 0.04f), col(250, 236, 200, 1), hn);
        }
    }
    int ts = ctx == X_TITLE && !g_editing ? title_screen() : 0;
    if (ts == 1 || (ts == 2 && !g_file_picked)) {  // what a tap does (past the file select: the game's own)
        const char *t = ts == 2 ? "Tap a file to play" : "Tap to start";
        // (the title screen: on its "Press L + R")
        float ty = ts == 2 ? h - 40 * tt : h * 0.735f;
        // the menus' own framed panel, solid (over the title's "Press L + R": a tap does that here), the words fading gently
        float fade = 0.72f + 0.28f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 6.2831853f / 1.8f));
        const PlasticTheme &pt = plastic_theme();
        ImVec2 ts_sz = menu_font->CalcTextSizeA(40 * tt, 1e9f, 0, t);
        float bx = fmaxf(ts_sz.x * 0.5f + 40 * tt, ts == 1 ? h * 0.175f : 0.0f), by = fmaxf(ts_sz.y * 0.5f + 16 * tt, ts == 1 ? h * 0.062f : 0.0f);
        if (ty + by > h - 8 * tt) ty = h - 8 * tt - by;
        ImVec2 b0(w * 0.5f - bx, ty - by), b1(w * 0.5f + bx, ty + by);
        ImVec4 p1 = pt.panel, p2 = pt.panel2;
        p1.w = p2.w = 1;
        float k = tt * 0.7f, in = 7 * k, fw = 13 * k;
        plastic_add(dl, b0, b1, 26 * k, p1, PK_SLAB, 14 * k, 0.3f, p2, 0, pt.mottle * tt);
        plastic_add(dl, ImVec2(b0.x + in, b0.y + in), ImVec2(b1.x - in, b1.y - in), 20 * k, pt.frame, PK_RING, 7 * k, 1.0f, pt.frame_inner, fw);
        g_title_box = true;
        label_text(dl, menu_font, 40 * tt, ImVec2(w * 0.5f, ty), t, col(255, 255, 255, fade), true);
    }
    if ((want & 1) && (g_editing || !hidden(C_STICK))) {
        const Control &s = g_ctl[C_STICK];
        ImVec2 sc(g_stick_cx, g_stick_cy);
        float sa = a * (g_editing && hidden(C_STICK) ? 0.35f : 1.0f);
        bool ssel = g_editing && g_sel == C_STICK;
        int32_t lx = g_editing ? 0 : g_lx.load(), ly = g_editing ? 0 : g_ly.load();
        if (g_settings.touch_dpad) {
            if (!dpad_disc(dl, sc, s.r, lx, ly, sa, th, ssel)) dpad(dl, sc, s.r, lx, ly, sa, th, ssel);
        } else if (!compass_disc(dl, sc, s.r, ImVec2(knob_x, knob_y), sa, th, ssel))
            compass(dl, sc, s.r, ImVec2(knob_x, knob_y), sa, th, ssel);
    }
    if (g_editing) {  // the editor: the hint and Default / Cancel / Done at the top, - and + by the selected control
        // (in the menus' look: Baloo 2, brown pills edged in tan, Done in gold)
        ImU32 edge = col(200, 185, 126, 1), cream = col(255, 240, 216, 1), ink = col(74, 52, 26, 1);
        auto pill = [&](ImVec2 p0, ImVec2 p1, bool gold) {
            float rr = (p1.y - p0.y) * 0.5f;
            dl->AddRectFilled(ImVec2(p0.x + 3 * th, p0.y + 6 * th), ImVec2(p1.x + 3 * th, p1.y + 6 * th), col(0, 0, 0, 0.40f), rr);
            dl->AddRectFilled(p0, p1, gold ? col(214, 174, 88, 1) : col(108, 79, 55, 1), rr);
            dl->AddRectFilled(ImVec2(p0.x + 3 * th, p0.y + 3 * th), ImVec2(p1.x - 3 * th, p0.y + (p1.y - p0.y) * 0.5f),
                              gold ? col(244, 212, 130, 1) : col(134, 102, 77, 1), rr, ImDrawFlags_RoundCornersTop);  // (lit from above)
            dl->AddRect(p0, p1, gold ? col(255, 238, 178, 1) : edge, rr, 0, 3 * th);
        };
        const char *tip = "Drag the controls to move them. Tap one to resize it.";
        float hs = 30 * tt;
        ImVec2 hz = menu_font->CalcTextSizeA(hs, 1e9f, 0, tip);
        float hy = h * 0.115f, hh = hs * 1.5f, hw = hz.x + hs * 1.6f;
        dl->AddRectFilled(ImVec2((w - hw) * 0.5f, hy - hh * 0.5f), ImVec2((w + hw) * 0.5f, hy + hh * 0.5f), col(59, 48, 17, 0.90f), hh * 0.5f);
        dl->AddRect(ImVec2((w - hw) * 0.5f, hy - hh * 0.5f), ImVec2((w + hw) * 0.5f, hy + hh * 0.5f), edge, hh * 0.5f, 0, 2.5f * th);
        dl->AddText(menu_font, hs, ImVec2((w - hz.x) * 0.5f, hy - hz.y * 0.5f), cream, tip);
        float bh = 56 * tt, gap = 16 * tt, fs = 32 * tt, minw = 150 * tt, tot = 0;
        for (int i = 0; i < 3; i++) {
            g_bar_w[i] = fmaxf(menu_font->CalcTextSizeA(fs, 1e9f, 0, kEditBar[i]).x + 44 * tt, minw);
            tot += g_bar_w[i] + (i ? gap : 0);
        }
        float x = (w - tot) * 0.5f;
        g_bar_y = hy + hh * 0.5f + 14 * tt, g_bar_h = bh;
        for (int i = 0; i < 3; i++) {
            g_bar_x[i] = x;
            pill(ImVec2(x, g_bar_y), ImVec2(x + g_bar_w[i], g_bar_y + bh), i == 2);
            ImVec2 tz = menu_font->CalcTextSizeA(fs, 1e9f, 0, kEditBar[i]);
            dl->AddText(menu_font, fs, ImVec2(x + (g_bar_w[i] - tz.x) * 0.5f, g_bar_y + (bh - tz.y) * 0.5f), i == 2 ? ink : cream, kEditBar[i]);
            x += g_bar_w[i] + gap;
        }
        if (g_sel >= 0) {  // - and +: side by side above the control (below it when there's no room), on the screen
            const Control &c = g_ctl[g_sel];
            ImVec2 cc = g_sel == C_STICK ? ImVec2(g_stick_cx, g_stick_cy) : ImVec2(c.x, c.y);
            float cr = c.pill ? c.r * 1.2f : c.r, br = 30 * tt, sp = 12 * tt, m = 8 * tt;
            float ty = cc.y - cr - sp - br;
            if (ty - br < g_bar_y + bh + m) ty = cc.y + cr + sp + br;
            float tx = clampf(cc.x, 2 * br + sp * 0.5f + m, w - 2 * br - sp * 0.5f - m);
            g_tool_r = br;
            for (int k = 0; k < 2; k++) {
                g_tool_x[k] = tx + (k ? 1 : -1) * (br + sp * 0.5f), g_tool_y[k] = ty;
                ImVec2 p(g_tool_x[k], ty);
                pill(ImVec2(p.x - br, p.y - br), ImVec2(p.x + br, p.y + br), false);
                float u = br * 0.42f, lw = 5 * th;
                dl->AddLine(ImVec2(p.x - u, p.y), ImVec2(p.x + u, p.y), cream, lw);
                if (k) dl->AddLine(ImVec2(p.x, p.y - u), ImVec2(p.x, p.y + u), cream, lw);
            }
        }
    }
}
#endif  // PLAT_TOUCH
