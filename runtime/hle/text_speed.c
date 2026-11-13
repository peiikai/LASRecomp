// Text speed (General tab, issue #681): how fast message text appears, through the two hook sites of tools/patches.py
// (TEXT_RATE_SITE, TEXT_SNAP_SITE). A message pane plays its text as a layout animation at a step it is given as each
// window opens; Fast plays it at a larger step, Instant puts the page's end on screen at once (the pane's own branch
// for text already at the end). Neither touches the window's states, so a page still waits for the button, and
// questions, item text and timed lines run as in the game.
#include <string.h>

#include "../settings.h"

#define TEXT_FAST_MULT 3.0f  // Fast: three times the game's step

// TEXT_RATE_SITE - str w8, [x0, #0x284]: the step the game stores (2.0f) -> the one to store
uint32_t rt_text_rate(uint32_t game_bits) {
    if (g_settings.text_speed != TEXT_SPEED_FAST) return game_bits;
    float step;
    memcpy(&step, &game_bits, 4);
    step *= TEXT_FAST_MULT;
    uint32_t bits;
    memcpy(&bits, &step, 4);
    return bits;
}

// TEXT_SNAP_SITE - b.lt: the text is short of the page's end. 1: skip the branch (the pane sets its frame to the end).
int rt_text_snap(void) { return g_settings.text_speed == TEXT_SPEED_INSTANT; }
