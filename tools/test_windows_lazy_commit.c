// Windows guest memory: chunks outside a committed region are committed on first touch by the vectored exception
// handler (runtime/platform/windows.c lazy_commit). When two threads fault on the same new chunk at once, the second
// one finds it already committed by the first: that must retry the access, not crash. A page protected on purpose
// (plat_protect, LARECOMP_WATCH) must still be left to the fault watch.
#undef NDEBUG
#include <assert.h>
#include "../runtime/platform/windows.c"

// The rest of the runtime, which the code under test doesn't reach
void rt_crash_explain(char *out, size_t n, const char *log_path) { (void)out; (void)n; (void)log_path; abort(); }
bool rt_fault_watch(uint64_t fault_addr, uint64_t host_pc) { (void)fault_addr; (void)host_pc; abort(); }
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) {
    (void)what; (void)fault_addr; (void)host_pc; (void)host_lr; abort();
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) { (void)pc; (void)offset; return NULL; }

#define THREADS 8
#define CHUNKS 512
static uint64_t g_base;
static volatile LONG g_arrived, g_round;

// All threads leave together, then write into the same untouched chunk at once
static void barrier(LONG round) {
    if (InterlockedIncrement(&g_arrived) == THREADS * (round + 1)) InterlockedExchange(&g_round, round + 1);
    while (g_round <= round) YieldProcessor();
}

static DWORD WINAPI toucher(LPVOID arg) {
    uint32_t t = (uint32_t)(uintptr_t)arg;
    for (LONG k = 0; k < CHUNKS; k++) {
        barrier(k);
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(g_base + (uint64_t)k * COMMIT_CHUNK + t * 4096 % COMMIT_CHUNK);
        *p = 0x1000u * t + (uint32_t)k;
    }
    return 0;
}

int main(void) {
    const uint64_t size = (uint64_t)(CHUNKS + 2) * COMMIT_CHUNK;
    void *probe = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);  // (a free range for the window)
    assert(probe);
    VirtualFree(probe, 0, MEM_RELEASE);
    uint64_t lo = (uint64_t)(uintptr_t)probe;
    assert(plat_reserve_guest(lo, lo + size));

    // The state the losing thread sees: its chunk is committed read-write by now. Retry, don't crash.
    uint64_t last = lo + (uint64_t)CHUNKS * COMMIT_CHUNK;
    plat_commit(last, COMMIT_CHUNK);
    assert(lazy_commit(last + 0x123));
    // Protected on purpose: not ours to retry
    uint64_t watched = last + COMMIT_CHUNK;
    assert(plat_protect(watched, 4096, PLAT_PROT_READ) == 0);
    assert(!lazy_commit(watched));
    // Outside the window: not ours either
    assert(!lazy_commit(lo + size));

    g_base = lo;
    HANDLE th[THREADS];
    for (uint32_t t = 0; t < THREADS; t++) {
        th[t] = CreateThread(NULL, 0, toucher, (LPVOID)(uintptr_t)t, 0, NULL);
        assert(th[t]);
    }
    assert(WaitForMultipleObjects(THREADS, th, TRUE, 60000) == WAIT_OBJECT_0);
    for (uint32_t t = 0; t < THREADS; t++) CloseHandle(th[t]);
    for (uint32_t k = 0; k < CHUNKS; k++)
        for (uint32_t t = 0; t < THREADS; t++)
            assert(*(uint32_t *)(uintptr_t)(lo + (uint64_t)k * COMMIT_CHUNK + t * 4096 % COMMIT_CHUNK) == 0x1000u * t + k);
    printf("lazy commit: %d threads x %d chunks, ok\n", THREADS, CHUNKS);
    return 0;
}
