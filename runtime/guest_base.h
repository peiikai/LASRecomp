// Base of the guest address window. The desktops fix it at compile time (LARECOMP_GUEST_BASE, the Makefile's
// GUEST_BASE: 0x70_0000_0000, the host allocator stays below it); with LARECOMP_DYNAMIC_BASE (iOS: no fixed address
// is free on every device and version) the runtime picks it at start-up, before any guest code runs. The generated
// code (tools/gen.py) writes every address as GUEST_ADDR(<offset>) (runtime/cpu.h) and so is the same either way; only the
// runtime knows where the window went. Guest addresses must fit in 39 bits (the Switch's address space).
#pragma once
#include <stdint.h>
#include "cpu.h"

#define GUEST_BASE GUEST_ADDR(0)
#define MAIN_BASE  GUEST_BASE
#define SDK_BASE   (GUEST_BASE + 0x0004000000ull)
#if LARECOMP_DYNAMIC_BASE
void guest_base_set(uint64_t base);  // once, before any guest code (runtime/main.c reserve_guest_space)
#endif

// The game-address tables (runtime/game_addresses/*.h) hold the Switch's addresses, main at 0x70_0000_0000; an
// address of the game at this build's base (sizes, below the window, stay as they are: runtime/game_addresses.c).
#define GUEST_SWITCH_BASE 0x7000000000ull
#define GUEST_FROM_SWITCH(a) ((uint64_t)(a) >= GUEST_SWITCH_BASE ? (uint64_t)(a) - GUEST_SWITCH_BASE + GUEST_BASE : (uint64_t)(a))
