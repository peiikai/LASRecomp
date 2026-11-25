// Exercise the soft page write tracking (runtime/platform/soft_tracking.h: mprotect + a fault handler), the Linux
// host's write tracking over the guest heap as driver memory (RADV, e.g. the Steam Deck). Needs 4K pages: Linux x86-64,
// or macOS as x86-64 under Rosetta (clang -arch x86_64).
//   clang -O2 -pthread tools/test_soft_tracking.c -o build/test-soft-tracking && build/test-soft-tracking
// 1. #883: a write faults on a page another thread is opening; before it sees the page open, a query arms it again.
//    The write must open the page itself, not be taken for a real fault (the game crashed).
// 2. Writers on a few pages and a thread querying them, for a while: every fault is ours, every write lands.
#define _GNU_SOURCE
#include "../runtime/platform/soft_tracking.h"
#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define NPAGES 8
static volatile uint32_t *g_mem;
static _Atomic int g_in_handler;

static void handler(int sig, siginfo_t *si, void *ctx) {
    (void)sig, (void)ctx;
    atomic_store(&g_in_handler, 1);
    if (soft_fault((uint64_t)(uintptr_t)si->si_addr)) return;
    static const char msg[] = "FAIL: a write to a tracked page was taken for a real fault\n";
    write(2, msg, sizeof msg - 1);
    _exit(1);
}

static void *write_page0(void *unused) {
    (void)unused;
    g_mem[0] = 0x883;
    return NULL;
}

static void rearmed_while_opening(void) {
    uint64_t lo = (uint64_t)(uintptr_t)g_mem, pages[NPAGES];
    soft_written_pages(lo, NPAGES * SOFT_PAGE, pages, NPAGES);  // (arms and protects every page)
    // Another thread has taken page 0 and is opening it: the page is still protected.
    atomic_store(&g_soft_state[0], SOFT_RELEASING);
    pthread_t t;
    pthread_create(&t, NULL, write_page0, NULL);
    while (!atomic_load(&g_in_handler)) sched_yield();
    usleep(50000);  // (the writer now waits for the page to open)
    // That thread opened it and a query armed it again (and protected it), all before the writer looked again.
    atomic_store(&g_soft_state[0], SOFT_ARMED);
    pthread_join(t, NULL);
    assert(g_mem[0] == 0x883);
    assert(atomic_load(&g_soft_state[0]) == SOFT_OPEN);
    int n = soft_written_pages(lo, NPAGES * SOFT_PAGE, pages, NPAGES);
    assert(n == 1 && pages[0] == lo);
    printf("ok: a page armed again while a write waited for it to open\n");
}

static _Atomic int g_stop;
static void *writer(void *arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg, i = 0;
    while (!atomic_load(&g_stop)) {
        uint32_t w = (i++ % 3) * (SOFT_PAGE / 4) + id;  // (pages 0-2, each writer its own word)
        g_mem[w] = i;
        assert(g_mem[w] == i);
    }
    return NULL;
}

static void stress(void) {
    enum { WRITERS = 8 };
    uint64_t lo = (uint64_t)(uintptr_t)g_mem, pages[NPAGES];
    pthread_t t[WRITERS];
    for (int i = 0; i < WRITERS; i++) pthread_create(&t[i], NULL, writer, (void *)(uintptr_t)(i + 1));
    uint64_t end = plat_now_ns() + 2000000000ull, queries = 0, written = 0;
    while (plat_now_ns() < end) {
        int n = soft_written_pages(lo, NPAGES * SOFT_PAGE, pages, NPAGES);
        assert(n >= 0 && n <= 3);
        written += (uint64_t)n, queries++;
    }
    atomic_store(&g_stop, 1);
    for (int i = 0; i < WRITERS; i++) pthread_join(t[i], NULL);
    assert(written > 0);
    printf("ok: %d writers and a querying thread for 2 s (%llu queries, %llu pages written)\n", WRITERS,
           (unsigned long long)queries, (unsigned long long)written);
}

int main(void) {
    if (sysconf(_SC_PAGESIZE) != SOFT_PAGE) {
        fprintf(stderr, "needs 4K pages (page size %ld)\n", sysconf(_SC_PAGESIZE));
        return 1;
    }
    void *m = mmap(NULL, NPAGES * SOFT_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(m != MAP_FAILED);
    g_mem = m;
    struct sigaction sa = {0};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);  // (macOS)
    plat_soft_write_tracking((uint64_t)(uintptr_t)m, (uint64_t)(uintptr_t)m + NPAGES * SOFT_PAGE);
    assert(g_soft_state);
    rearmed_while_opening();
    stress();
    return 0;
}
