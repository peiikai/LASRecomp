// Exercise the real Text speed hooks (#681) the way the generated code at the two sites runs them.
#include <assert.h>
#include <string.h>
#include "../runtime/hle/text_speed.c"

Settings g_settings;

static float as_float(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

int main(void) {
    const uint32_t game = 0x40000000;  // the 2.0f the game stores as a window opens
    // Normal: the game's step and its own branch, untouched; also for a value out of range (an edited file)
    for (int speed = -1; speed <= N_TEXT_SPEEDS; speed += N_TEXT_SPEEDS + 1) {
        g_settings.text_speed = speed;
        assert(rt_text_rate(game) == game && !rt_text_snap());
    }
    g_settings.text_speed = TEXT_SPEED_NORMAL;
    assert(rt_text_rate(game) == game && !rt_text_snap());
    // Fast: a larger step, the game's branch (the text still plays character by character)
    g_settings.text_speed = TEXT_SPEED_FAST;
    assert(as_float(rt_text_rate(game)) == 2.0f * TEXT_FAST_MULT && TEXT_FAST_MULT > 1 && !rt_text_snap());
    assert(rt_text_rate(0) == 0);  // (a pane with no step keeps none)
    // Instant: the game's step (the button's B skip is unchanged), the "at the page's end" path at once
    g_settings.text_speed = TEXT_SPEED_INSTANT;
    assert(rt_text_rate(game) == game && rt_text_snap() == 1);
    return 0;
}
