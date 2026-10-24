// Base of the guest address window. One compile-time parameter (LARECOMP_GUEST_BASE, the Makefile's GUEST_BASE)
// moves the whole layout: macOS keeps 0x70_0000_0000 (the host allocator stays below it), iOS processes top out
// near 0xF_C000_0000 and use a lower base. tools/game_addresses.py reads the same value from LARECOMP_GUEST_BASE.
// Guest addresses must fit in 39 bits (the Switch's address space), and generated code (tools/gen.py) bakes the
// base into its literals: the game code library only matches a runtime built with the same base.
#pragma once
#include <stdint.h>

#ifndef LARECOMP_GUEST_BASE
#define LARECOMP_GUEST_BASE 0x7000000000
#endif
#define GUEST_BASE ((uint64_t)LARECOMP_GUEST_BASE)
#define MAIN_BASE  GUEST_BASE
#define SDK_BASE   (GUEST_BASE + 0x0004000000ull)

// The game-address tables (runtime/game_addresses/*.h) hold the Switch's addresses, main at 0x70_0000_0000; an
// address of the game at this build's base (sizes, below the window, stay as they are: runtime/game_addresses.c).
#define GUEST_SWITCH_BASE 0x7000000000ull
#define GUEST_FROM_SWITCH(a) ((uint64_t)(a) >= GUEST_SWITCH_BASE ? (uint64_t)(a) - GUEST_SWITCH_BASE + GUEST_BASE : (uint64_t)(a))
