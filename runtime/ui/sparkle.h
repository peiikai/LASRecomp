// macOS: the Sparkle updater (runtime/ui/sparkle.mm). Elsewhere these do nothing.
#pragma once
#include "../platform.h"

// Loads Sparkle (when the app has it) and starts it; check_now: the one background check of this launch. false: none.
bool sparkle_start(bool check_now);
bool sparkle_available(void);
// The update dialog's Update: Sparkle checks now and shows its own window
void sparkle_check_now(void);
