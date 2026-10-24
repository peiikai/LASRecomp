// iOS host layer (runtime/platform.h): macOS's Mach / POSIX pieces, minus what an iOS app can't do (start
// processes, re-exec itself, show an alert from any thread). The data folder is the app's Documents (runtime/paths.c),
// visible in Files with UIFileSharingEnabled: game files, saves, seeds and the log go there.
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach/vm_map.h>
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
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"
#include "posix_log.h"

void rt_crash_explain(char *out, size_t n, const char *log_path);  // runtime/crash_explain.c
const char *plat_ios_documents(void);                             // runtime/platform/ios_glue.m
const char *plat_ios_os_version(void);                            // ("26.0.1")

static mach_timebase_info_data_t tb;
static char g_log_path[1024];

static void sysctl_str(const char *name, char *out, size_t n) {
    size_t len = n;
    if (sysctlbyname(name, out, &len, NULL, 0) != 0) out[0] = 0;
    else out[n - 1] = 0;
}
static int64_t sysctl_int(const char *name) {
    int64_t v = 0;
    size_t len = sizeof v;
    return sysctlbyname(name, &v, &len, NULL, 0) == 0 ? v : 0;
}

// The log's first line: the iOS version and the device model (hw.machine: "iPhone16,1"; no CPU brand string here)
void plat_system_info(PlatSysInfo *si) {
    struct utsname u;
    char machine[48] = "";
    if (uname(&u) == 0) snprintf(machine, sizeof machine, "%s", u.machine);
    snprintf(si->os, sizeof si->os, "iOS %s (%s)", plat_ios_os_version(), machine);
    sysctl_str("hw.machine", si->cpu, sizeof si->cpu);
    si->cores = (int)sysctl_int("hw.physicalcpu");
    si->threads = (int)sysctl_int("hw.logicalcpu");
    int64_t p = sysctl_int("hw.perflevel0.physicalcpu"), e = sysctl_int("hw.perflevel1.physicalcpu");
    if (p && e) snprintf(si->cores_detail, sizeof si->cores_detail, "%dP + %dE", (int)p, (int)e);
    si->ram = (uint64_t)sysctl_int("hw.memsize");
    si->package = ".ipa";
}

bool plat_onedrive_synced(const char *dir) { (void)dir; return false; }

// stdout / stderr -> lasrecomp.log in Documents (five backups, posix_log.h), unless LARECOMP_LOG_STDERR is set
// (Xcode / devicectl console runs).
void plat_init(void) {
    const char *dir = plat_ios_documents();
    if (!getenv("LARECOMP_LOG_STDERR") && dir && *dir) {
        mkdir(dir, 0755);
        session_log_open(dir, g_log_path, sizeof g_log_path);
    }
    // (arguments and environment as on the desktop: `xcrun devicectl device process launch --environment-variables
    // '{"LARECOMP_...":"1"}' <bundle id> --play`; from the home screen there are none)
    if (getenv("LARECOMP_IOS_PYTEST")) {  // the embedded Python's self-test (ios_python.m): its log lines, then on
        extern void plat_ios_python_selftest(void);
        plat_ios_python_selftest();
    }
}

// The window is below the process's VA limit (runtime/guest_base.h: iOS builds use a low GUEST_BASE); pages are
// backed as touched. The limit depends on the signature: without com.apple.developer.kernel.increased-memory-limit
// the kernel gives the process a smaller address space and the 12.5 GB window doesn't fit (measured on an iPhone
// 15 Pro: the same app starts with the entitlement and fails here without it), so a failure says what the process
// got, for the log of a sideloaded build whose signing tool dropped the entitlement.
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    vm_address_t a = (vm_address_t)lo;
    kern_return_t kr = vm_allocate(mach_task_self(), &a, (vm_size_t)(hi - lo), VM_FLAGS_FIXED);
    if (kr == KERN_SUCCESS) return true;
    fprintf(stderr, "[vm] vm_allocate(%llx..%llx, FIXED) = %d%s\n", (unsigned long long)lo, (unsigned long long)hi, kr,
            kr == KERN_NO_SPACE ? " (KERN_NO_SPACE)" : kr == KERN_INVALID_ADDRESS ? " (KERN_INVALID_ADDRESS)" : "");
    for (uint64_t want = 64ull << 30; want >= 1ull << 30; want >>= 1) {  // (the largest free run, 64 GB down to 1 GB)
        vm_address_t any = 0;
        if (vm_allocate(mach_task_self(), &any, (vm_size_t)want, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
            fprintf(stderr, "[vm] largest free run: %llu GB at %llx; the window needs %llu GB at %llx. Is the app signed "
                            "with com.apple.developer.kernel.increased-memory-limit (runtime/platform/ios.entitlements)?\n",
                    (unsigned long long)(want >> 30), (unsigned long long)any, (unsigned long long)((hi - lo) >> 30),
                    (unsigned long long)lo);
            vm_deallocate(mach_task_self(), any, (vm_size_t)want);
            break;
        }
    }
    return false;
}

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    vm_address_t a = (vm_address_t)lo;
    return vm_allocate(mach_task_self(), &a, (vm_size_t)(hi - lo), VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS;
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

void plat_thread_realtime(uint64_t period_ns) {
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t period = period_ns * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)period,
        .computation = (uint32_t)(period / 20),
        .constraint = (uint32_t)(period / 4),
        .preemptible = 1,
    };
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

// As on macOS: the kernel drops the time-constraint policy of a thread that overruns its budget; set again when gone.
int plat_thread_realtime_check(uint64_t period_ns) {
    thread_time_constraint_policy_data_t pol;
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t def = FALSE;
    kern_return_t kr = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                        (thread_policy_t)&pol, &count, &def);
    if (kr != KERN_SUCCESS) return -1;
    if (!def) return 0;
    plat_thread_realtime(period_ns);
    return 1;
}

int plat_cpu_count(void) { return 0; }

// As on macOS: the frame-critical guest threads on the performance cores.
void plat_thread_guest(int prio) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_THREAD_QOS") || atoi(getenv("LARECOMP_THREAD_QOS")) != 0;
    if (on) pthread_set_qos_class_self_np(prio <= 44 ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
}

bool plat_exe_path(char *buf, size_t size) {
    uint32_t len = (uint32_t)size;
    return _NSGetExecutablePath(buf, &len) == 0;
}

void plat_exec(char *const argv[]) { (void)argv; }  // (no exec on iOS: the caller reports the failure)

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

// A fatal error: explained into the log (an alert needs the main thread and a window, which may be what failed).
void plat_fatal_notice(void) {
    char text[1600];
    rt_crash_explain(text, sizeof text, g_log_path);
    fprintf(stderr, "%s\n", text);
    fflush(stderr);
}

void plat_notice(const char *text) { fprintf(stderr, "[notice] %s\n", text); }

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++) fprintf(stderr, "  %s\n", syms[i]);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }
    uint64_t fa = (uint64_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr;
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

// (child processes: runtime/platform/ios_python.m - Python scripts as threads of the embedded interpreter)

#include <ftw.h>
static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }
