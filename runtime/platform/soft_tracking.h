// Soft page write tracking for the Linux host layer (linux.c), on its own so tools/test_soft_tracking.c can run it
// (also on macOS: only mprotect and a fault handler). Included once, by linux.c.
#pragma once
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

uint64_t plat_now_ns(void);  // (runtime/platform.h; not included: off Linux it stubs plat_os_write for the test)

// ---- soft page write tracking: mprotect + SIGSEGV, where the kernel's can't run (LARECOMP_SOFT_WRITE_TRACKING=0: off)
// The guest heap as driver memory (exec.c guest_heap_dmabuf: RADV) is no anonymous memory, so userfaultfd can't watch
// it; there a query instead protects the pages it covered read-only, and the CPU's first write to one faults:
// crash_handler (soft_fault) records the page and gives it its write access back. The GPU's own mapping of the memory
// is not the CPU's page table, so the GPU never notices. Only pages a query covered ever fault, and each at most once
// between two queries. The OS's own writes (file reads into the heap) would fail on a protected page instead of
// faulting: plat_os_write unprotects first.
// Page states: 0 never protected (not ours), 1 protected, 2 being given back, 3 writable again (was ours).
enum { SOFT_NONE, SOFT_ARMED, SOFT_RELEASING, SOFT_OPEN };
static _Atomic uint8_t *g_soft_state, *g_soft_written;  // one byte per 4K page of [g_soft_lo, g_soft_hi)
static uint64_t g_soft_lo, g_soft_hi;
static _Atomic uint64_t g_soft_faults, g_soft_queries, g_soft_protects;
#define SOFT_PAGE 4096ull

void plat_soft_write_tracking(uint64_t lo, uint64_t hi) {
    const char *e = getenv("LARECOMP_SOFT_WRITE_TRACKING");
    if ((e && !atoi(e)) || g_soft_state) return;
    uint64_t n = (hi - lo) / SOFT_PAGE;
    g_soft_state = calloc(n, 1), g_soft_written = calloc(n, 1);  // (untouched parts of these cost nothing either)
    if (!g_soft_state || !g_soft_written) { free((void *)g_soft_state), free((void *)g_soft_written); g_soft_state = NULL; return; }
    g_soft_lo = lo, g_soft_hi = hi;
    fprintf(stderr, "[plat] soft page write tracking on (mprotect + SIGSEGV) over %llx-%llx\n", (unsigned long long)lo,
            (unsigned long long)hi);
}

// Give page p its write access back and count it written (the fault handler and plat_os_write). False: not ours.
static bool soft_release(uint64_t p) {
    for (;;) {
        uint8_t st = SOFT_ARMED;
        if (atomic_compare_exchange_strong(&g_soft_state[p], &st, SOFT_RELEASING)) {
            atomic_store(&g_soft_written[p], 1);  // (before the page opens: a query that sees it open sees it written)
            mprotect((void *)(uintptr_t)(g_soft_lo + p * SOFT_PAGE), SOFT_PAGE, PROT_READ | PROT_WRITE);
            atomic_store(&g_soft_state[p], SOFT_OPEN);
            atomic_fetch_add(&g_soft_faults, 1);
            return true;
        }
        if (st != SOFT_RELEASING) return st == SOFT_OPEN;  // (open: the write retries; SOFT_NONE: a real fault)
        // Another thread opens it: wait, then look again. If this thread is descheduled meanwhile, a query can arm the
        // page again before it sees it open; then it's armed (and protected) and this thread opens it. Taking that for
        // "not ours" crashed the game on a write the game was allowed to make (#883: a Steam Deck, RADV).
        while (atomic_load(&g_soft_state[p]) == SOFT_RELEASING) {}
    }
}

static bool soft_fault(uint64_t fa) {
    if (!g_soft_state || fa < g_soft_lo || fa >= g_soft_hi) return false;
    return soft_release((fa - g_soft_lo) / SOFT_PAGE);
}

void plat_os_write(uint64_t addr, uint64_t size) {
    if (!g_soft_state || !size || addr >= g_soft_hi || addr + size <= g_soft_lo) return;
    uint64_t a = addr < g_soft_lo ? g_soft_lo : addr, b = addr + size > g_soft_hi ? g_soft_hi : addr + size;
    for (uint64_t p = (a - g_soft_lo) / SOFT_PAGE; p <= (b - 1 - g_soft_lo) / SOFT_PAGE; p++)
        if (atomic_load(&g_soft_state[p]) == SOFT_ARMED) soft_release(p);
}

// The kernel driver about to pin [addr, addr+size) for writing (a host-pointer import: Mali's kbase pins with write
// access, which fails on a page soft tracking protected read-only - the whole import is refused). Queries stop
// protecting (they report every page they cover as written meanwhile: the safe side), the ones already running are
// waited out, and the range's protected pages are opened (counted written). plat_os_write_end: queries protect again.
static _Atomic int g_soft_hold, g_soft_inflight;
void plat_os_write_begin(uint64_t addr, uint64_t size) {
    if (!g_soft_state) return;
    atomic_fetch_add(&g_soft_hold, 1);
    while (atomic_load(&g_soft_inflight)) sched_yield();
    plat_os_write(addr, size);
}
void plat_os_write_end(void) {
    if (g_soft_state) atomic_fetch_sub(&g_soft_hold, 1);
}

// plat_written_pages over soft tracking: protect the range's open pages first, then take the written marks, so a write
// between the two is either already marked or faults after the protection.
static int soft_written_pages_(uint64_t addr, uint64_t size, uint64_t *pages, int max);
static int soft_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max) {
    if (addr < g_soft_lo || addr + size > g_soft_hi || max <= 0) return -1;
    atomic_fetch_add(&g_soft_inflight, 1);
    int n;
    if (atomic_load(&g_soft_hold)) {  // (an import is pinning pages: protect nothing, every page counts as written)
        // The marks stay set: the pages stay open meanwhile, so a CPU write to one now neither faults nor marks it; the
        // first query after the hold arms the page and must still report it (once more: the safe side).
        uint64_t first = (addr - g_soft_lo) / SOFT_PAGE, last = (addr + size - 1 - g_soft_lo) / SOFT_PAGE;
        n = 0;
        for (uint64_t p = first; p <= last && n < max; p++) { atomic_store(&g_soft_written[p], 1); pages[n++] = g_soft_lo + p * SOFT_PAGE; }
    } else n = soft_written_pages_(addr, size, pages, max);
    atomic_fetch_sub(&g_soft_inflight, 1);
    return n;
}
static int soft_written_pages_(uint64_t addr, uint64_t size, uint64_t *pages, int max) {
    uint64_t first = (addr - g_soft_lo) / SOFT_PAGE, last = (addr + size - 1 - g_soft_lo) / SOFT_PAGE;
    for (uint64_t p = first; p <= last;) {  // runs of pages not protected: one mprotect each
        uint8_t st = atomic_load(&g_soft_state[p]);
        if (st == SOFT_ARMED || st == SOFT_RELEASING) { p++; continue; }
        uint64_t q = p;
        while (q <= last && (st = atomic_load(&g_soft_state[q])) != SOFT_ARMED && st != SOFT_RELEASING) q++;
        for (uint64_t i = p; i < q; i++) atomic_store(&g_soft_state[i], SOFT_ARMED);
        if (mprotect((void *)(uintptr_t)(g_soft_lo + p * SOFT_PAGE), (q - p) * SOFT_PAGE, PROT_READ)) {
            static int warned;
            if (warned++ < 5)
                fprintf(stderr, "[plat] soft tracking: mprotect %llx +%llu pages failed: %s\n",
                        (unsigned long long)(g_soft_lo + p * SOFT_PAGE), (unsigned long long)(q - p), strerror(errno));
        }
        atomic_fetch_add(&g_soft_protects, 1);
        p = q;
    }
    int n = 0;
    for (uint64_t p = first; p <= last; p++)
        if (atomic_load(&g_soft_written[p]) && atomic_exchange(&g_soft_written[p], 0)) {
            if (n < max) pages[n] = g_soft_lo + p * SOFT_PAGE;
            n++;
        }
    uint64_t q = atomic_fetch_add(&g_soft_queries, 1);
    static uint64_t last_log;
    uint64_t now = plat_now_ns();
    if (now - last_log > 5000000000ull) {
        fprintf(stderr, "[plat] soft tracking, last 5 s: %llu queries, %llu protects, %llu faults\n",
                    (unsigned long long)q, (unsigned long long)atomic_exchange(&g_soft_protects, 0),
                    (unsigned long long)atomic_exchange(&g_soft_faults, 0));
        atomic_store(&g_soft_queries, 0);
        last_log = now;
    }
    return n < max ? n : max;
}
