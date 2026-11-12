// Which side of a phone in landscape has the camera cutout: the menu pages keep their cushion on that side only.
#pragma once
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// l, r: the window's safe area insets, in, and what's left of them, out
static inline void touch_cushion(float *l, float *r, SDL_Window *win) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // iOS gives both sides the same inset in landscape, whichever has the notch: it's at the top of the phone held
    // upright, so the way the phone is turned says which
    if (*l == *r && *l > 0) {
        SDL_DisplayOrientation o = SDL_GetCurrentDisplayOrientation(SDL_GetDisplayForWindow(win));
        if (o == SDL_ORIENTATION_LANDSCAPE) *r = 0;
        else if (o == SDL_ORIENTATION_LANDSCAPE_FLIPPED) *l = 0;
    }
#else
    // Android's safe area also counts the gesture areas down both sides, the same width: the cutout's side is the wider
    (void)win;
    if (*l > *r) *r = 0;
    else if (*r > *l) *l = 0;
    else *l = *r = 0;
#endif
}

// The screen's rounded corners' radius, from the window's side insets (l, r): an iPhone in landscape reports its side
// inset at about the corners' radius. 0 elsewhere (a tablet's sides report none; Android's include the gesture areas).
static inline float touch_corner_radius(float l, float r) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return l > r ? l : r;
#else
    (void)l, (void)r;
    return 0;
#endif
}
