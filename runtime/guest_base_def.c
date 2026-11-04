// The guest base of a LARECOMP_DYNAMIC_BASE build (runtime/cpu.h): readers see `extern const uint64_t g_guest_base`,
// so this is the one place it is a plain writable word - set once by runtime/main.c before any guest code runs.
#include <stdint.h>
#if LARECOMP_DYNAMIC_BASE
uint64_t g_guest_base __attribute__((visibility("default")));
void guest_base_set(uint64_t base) { g_guest_base = base; }
#endif
