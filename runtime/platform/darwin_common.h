// What the macOS and iOS host layers share (runtime/platform.h): the Mach / POSIX pieces that are the same on both,
// and iCloud Drive.
// Included once by each one's host layer (macos.c, ios.c), as posix_log.h is by macos.c and linux.c.
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fts.h>
#include <ftw.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <time.h>

#include "../platform.h"

static mach_timebase_info_data_t tb;

static void sysctl_str(const char *name, char *out, size_t n) {
    size_t len = n;
    if (sysctlbyname(name, out, &len, NULL, 0) != 0) out[0] = 0;
    else out[n - 1] = 0;
}
static int64_t sysctl_int(const char *name) {
    int64_t v = 0;  // (hw.memsize is 64-bit, the counts 32-bit: little-endian, so either fits)
    size_t len = sizeof v;
    return sysctlbyname(name, &v, &len, NULL, 0) == 0 ? v : 0;
}

// plat_system_info's cores and memory
static void system_info_cores_ram(PlatSysInfo *si) {
    si->cores = (int)sysctl_int("hw.physicalcpu");
    si->threads = (int)sysctl_int("hw.logicalcpu");
    int64_t p = sysctl_int("hw.perflevel0.physicalcpu"), e = sysctl_int("hw.perflevel1.physicalcpu");
    if (p && e) snprintf(si->cores_detail, sizeof si->cores_detail, "%dP + %dE", (int)p, (int)e);
    si->ram = (uint64_t)sysctl_int("hw.memsize");
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

uint64_t plat_now_ns(void) {
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

uint64_t plat_thread_cpu_ns(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ? (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec : 0;
}

void plat_sleep_until_ns(uint64_t t) {
    if (!tb.denom) mach_timebase_info(&tb);
    mach_wait_until(t * tb.denom / tb.numer);
}

// Real-time (time-constraint) thread: exempt from the timer coalescing macOS applies to
// ordinary threads when the app isn't frontmost, which otherwise stretches 16ms waits to ~100ms.
void plat_thread_realtime(uint64_t period_ns) {
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t period = period_ns * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)period,
        .computation = (uint32_t)(period / 20),
        .constraint = (uint32_t)(period / 4),
        .preemptible = 1,
    };
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                                         THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) fprintf(stderr, "[plat] real-time thread policy refused: %d\n", (int)kr);
}
// After a late wait: is the time-constraint policy still in effect for this thread? The kernel drops it (back to
// timeshare, timers coalesced to ~100 ms when the app isn't frontmost) for a thread that overruns its computation
// budget; set again when found gone. Called only on the late path: no cost otherwise.
int plat_thread_realtime_check(uint64_t period_ns) {
    thread_time_constraint_policy_data_t pol;
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t def = FALSE;
    kern_return_t kr = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol, &count, &def);
    if (kr != KERN_SUCCESS) return -1;
    if (!def) return 0;
    plat_thread_realtime(period_ns);
    return 1;
}

// Guest threads start at the default QoS, and on a Mac with efficiency cores (an M1 Air: 4 + 4) the scheduler may run
// the game's frame-critical threads there - the main thread (priority 44) builds and submits every frame. Priorities
// 44 and more urgent get user-interactive (performance cores first), the rest user-initiated. LARECOMP_THREAD_QOS=0:
// the default class as before.
int plat_cpu_count(void) { return 0; }  // (no affinity masks here: the caller uses the machine's count)

void plat_thread_guest(int prio) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_THREAD_QOS") || atoi(getenv("LARECOMP_THREAD_QOS")) != 0;
    if (on) pthread_set_qos_class_self_np(prio <= 44 ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
}

bool plat_exe_path(char *buf, size_t size) {
    uint32_t len = (uint32_t)size;
    return _NSGetExecutablePath(buf, &len) == 0;
}

const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

bool plat_module_name(uint64_t pc, char *out, size_t n) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) return false;
    snprintf(out, n, "%s", di.dli_fname);
    return true;
}

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++) fprintf(stderr, "  %s\n", syms[i]);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die - the report
                                                                               // itself aborting looped, 5.7 GB log)
    uint64_t fa = (uint64_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr;
    { extern uint64_t g_fault_lr; g_fault_lr = lr; }
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path) == 0 || errno == ENOENT ? 0 : -1;  // (gone already: e.g. a ._ file macOS removed with its folder on exFAT)
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// A folder in iCloud Drive: its files may not be on the device, and a write there is a sync. The start screen
// warns, as it does for OneDrive on Windows. (Other providers - Google Drive, Dropbox - look just like On My iPhone
// from here: all of them are folders under some app group's File Provider Storage; no warning for those.)
const char *plat_cloud_synced(const char *dir) {
    static char name[128];
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)dir, (CFIndex)strlen(dir), true);
    if (!u) return NULL;
    CFBooleanRef ubi = NULL;
    bool synced = CFURLCopyResourcePropertyForKey(u, kCFURLIsUbiquitousItemKey, &ubi, NULL) && ubi && CFBooleanGetValue(ubi);
    if (ubi) CFRelease(ubi);
    if (synced) {
        // (the key has no CoreFoundation name; it is NSURLUbiquitousItemContainerDisplayNameKey's own string)
        CFStringRef shown = NULL;
        if (!CFURLCopyResourcePropertyForKey(u, CFSTR("NSURLUbiquitousItemContainerDisplayNameKey"), &shown, NULL) || !shown
            || !CFStringGetCString(shown, name, sizeof name, kCFStringEncodingUTF8) || !name[0])
            snprintf(name, sizeof name, "iCloud Drive");
        if (shown) CFRelease(shown);
    }
    CFRelease(u);
    return synced ? name : NULL;
}

// A file under dir (iCloud) the system has offloaded: the game would wait for each as it reads it. Read from the
// file system's own mark (SF_DATALESS), entry by entry. No folder answers for its files: a folder's
// NSURLUbiquitousItemDownloadingStatusKey stays Current with files in it offloaded, with Keep Downloaded set on it
// too (macOS then brings an offloaded file back, 20 to 60 s later). The same key read per file names the same
// files as the mark, but takes seconds (4800 entries: 6 to 7.6 s against 20 ms on an iPhone 15 Pro, 5.5 s against
// 13 ms on macOS 26).
bool plat_cloud_offloaded(const char *dir) {
    uint64_t t0 = plat_now_ns();
    char *paths[] = {(char *)dir, NULL};
    FTS *f = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!f) return false;
    int n = 0;
    bool off = false;
    for (FTSENT *e; !off && (e = fts_read(f));) {
        if (e->fts_info == FTS_DP) continue;
        n++;
        // (a folder with the mark hasn't its list here either: listing it would be a download)
        if ((e->fts_info == FTS_F || e->fts_info == FTS_D) && (e->fts_statp->st_flags & SF_DATALESS)) {
            fprintf(stderr, "[cloud] not on the device: %s\n", e->fts_path);
            off = true;
        }
    }
    fts_close(f);
    fprintf(stderr, "[cloud] %d entries checked in %.0f ms\n", n, (plat_now_ns() - t0) / 1e6);
    return off;
}
