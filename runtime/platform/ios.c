// iOS host layer (runtime/platform.h): macOS's Mach / POSIX pieces, minus what an iOS app can't do (start
// processes, re-exec itself, show an alert from any thread). The data folder is the app's Documents (runtime/paths.c),
// visible in Files with UIFileSharingEnabled: game files, saves, seeds and the log go there.
// The pieces shared with macOS (timers, the thread policies, symbols, the crash handler, plat_remove_tree, iCloud
// Drive) are darwin_common.h's.
#include <CoreFoundation/CoreFoundation.h>
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <os/proc.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "../paths.h"
#include "../platform.h"
#include "darwin_common.h"
#include "ios_internal.h"
#include "posix_log.h"

void rt_crash_explain(char *out, size_t n, const char *log_path);  // runtime/crash_explain.c

static char g_log_path[1024];

// plat_reserve_guest runs from a constructor, before plat_init has opened the log file: what it says is kept here
// and written into the log once there is one (launched from the home screen, stderr goes nowhere).
static char g_vm_report[16384];
static size_t g_vm_report_len;
static void __attribute__((format(printf, 1, 2))) vm_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_vm_report_len < sizeof g_vm_report - 1) {
        va_start(ap, fmt);
        int n = vsnprintf(g_vm_report + g_vm_report_len, sizeof g_vm_report - g_vm_report_len, fmt, ap);
        va_end(ap);
        if (n > 0) g_vm_report_len += (size_t)n < sizeof g_vm_report - g_vm_report_len ? (size_t)n : sizeof g_vm_report - 1 - g_vm_report_len;
    }
}

// The log's first line: the iOS version and the device model (hw.machine: "iPhone16,1"; no CPU brand string here)
void plat_system_info(PlatSysInfo *si) {
    struct utsname u;
    char machine[48] = "";
    if (uname(&u) == 0) snprintf(machine, sizeof machine, "%s", u.machine);
    snprintf(si->os, sizeof si->os, "iOS %s (%s)", plat_ios_os_version(), machine);
    sysctl_str("hw.machine", si->cpu, sizeof si->cpu);
    system_info_cores_ram(si);
    si->package = ".ipa";
}

// 1 / 0: the signature has / lacks com.apple.developer.kernel.increased-memory-limit, -1: unreadable. Without it the
// game may still start (the window fits on some devices), but iOS ends the app at a much lower footprint.
// SecTask is in Security.framework but not in the iOS SDK's headers. LARECOMP_TEST_NO_MEMORY_ENTITLEMENT=1 (test): 0.
static int entitlement_value(CFTypeRef (*value)(CFTypeRef, CFStringRef, CFErrorRef *), CFTypeRef task, CFStringRef key) {
    CFTypeRef v = value(task, key, NULL);
    if (!v) return 0;
    int r = CFGetTypeID(v) == CFBooleanGetTypeID() ? CFBooleanGetValue((CFBooleanRef)v) : 1;
    CFRelease(v);
    return r;
}
int plat_ios_memory_entitled(void) {
    static int r = -2;
    if (r != -2) return r;
    if (getenv("LARECOMP_TEST_NO_MEMORY_ENTITLEMENT")) return r = 0;
    r = -1;
    void *sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW);
    CFTypeRef (*create)(CFAllocatorRef) = sec ? (CFTypeRef (*)(CFAllocatorRef))dlsym(sec, "SecTaskCreateFromSelf") : NULL;
    CFTypeRef (*value)(CFTypeRef, CFStringRef, CFErrorRef *) =
        sec ? (CFTypeRef (*)(CFTypeRef, CFStringRef, CFErrorRef *))dlsym(sec, "SecTaskCopyValueForEntitlement") : NULL;
    CFTypeRef task = create && value ? create(NULL) : NULL;
    if (!task) return r;
    r = entitlement_value(value, task, CFSTR("com.apple.developer.kernel.increased-memory-limit"));
    if (r == 0 && entitlement_value(value, task, CFSTR("application-identifier")) == 0) r = -1;  // (nothing readable)
    CFRelease(task);
    return r;
}

// What iOS lets the app use before it ends it: what is left plus what is in use (the jetsam limit itself can't be read:
// memorystatus_control is EPERM in the sandbox). It rises once iOS turns Game Mode on. 0: not reported.
static uint64_t memory_limit(void) {
    uint64_t avail = os_proc_available_memory();
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (!avail || task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) != KERN_SUCCESS) return 0;
    return avail + vi.phys_footprint;
}

// stdout / stderr -> lasrecomp.log in the data folder, as on the desktop (five backups, posix_log.h; Documents when
// the chosen one isn't there), unless LARECOMP_LOG_STDERR is set (Xcode / devicectl console runs).
// LARECOMP_LOG_DOCUMENTS=1: in Documents whatever the data folder (where devicectl can pull it from).
void plat_init(void) {
    char dir[1024];
    if (getenv("LARECOMP_LOG_DOCUMENTS") || !rt_data_root_early(dir, sizeof dir))
        snprintf(dir, sizeof dir, "%s", plat_ios_documents());
    if (!getenv("LARECOMP_LOG_STDERR") && dir[0]) {
        mkdir(dir, 0755);
        session_log_open(dir, g_log_path, sizeof g_log_path);
        if (g_vm_report_len) fputs(g_vm_report, stderr);
    }
    raise_open_file_limit();
    int ent = plat_ios_memory_entitled();
    fprintf(stderr, "[memory] limit %llu MB (available + footprint), device %lld MB, increased-memory-limit %s\n",
            (unsigned long long)(memory_limit() >> 20), (long long)(sysctl_int("hw.memsize") >> 20),
            ent > 0 ? "yes" : ent == 0 ? "NO" : "unknown");
    // (arguments and environment as on the desktop: `xcrun devicectl device process launch --environment-variables
    // '{"LARECOMP_...":"1"}' <bundle id> --play`; from the home screen there are none)
    if (getenv("LARECOMP_IOS_PYTEST")) {  // the embedded Python's self-test (ios_python.m): its log lines, then on
        plat_ios_python_selftest();
    }
}

// The process's address space: the free gaps of 256 MB or more and the large regions, to the log (what the window
// could fit into, and what holds the rest: the dyld shared cache's tag-32 mapping, the kernel's tag-31 reserves).
// Where an iOS process has room differs by device and iOS version (iOS 26: the gap above 0x3_0000_0000; iOS 27: the
// shared cache spans 0x2_EA00_0000..0xF_C000_0000, with room only above it, which only
// com.apple.developer.kernel.extended-virtual-addressing opens), so the window is placed at start-up (plat_place_guest)
// and a failure reports the map for the next device.
static void vm_report(void) {
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS)
        vm_log("[vm] address space %llx..%llx (%llu GB)\n", (unsigned long long)vi.min_address,
               (unsigned long long)vi.max_address, (unsigned long long)((vi.max_address - vi.min_address) >> 30));
    vm_address_t addr = 0;
    uint64_t prev_end = 0;
    for (int n = 0; n < 4096; n++) {
        vm_size_t size;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t icnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &addr, &size, &depth, (vm_region_recurse_info_t)&info, &icnt) != KERN_SUCCESS) break;
        if (addr > prev_end && addr - prev_end >= 256ull << 20)
            vm_log("[vm]   free %llx..%llx (%llu MB)\n", prev_end, (unsigned long long)addr,
                   (unsigned long long)((addr - prev_end) >> 20));
        if (size >= 1ull << 30)  // (what holds the big ones: user tag, protection, share mode)
            vm_log("[vm]   used %llx..%llx (%llu MB) tag %u prot %x share %u\n", (unsigned long long)addr,
                   (unsigned long long)(addr + size), (unsigned long long)(size >> 20), info.user_tag, info.protection,
                   info.share_mode);
        prev_end = addr + size;
        addr = prev_end;
    }
    if (cnt && vi.max_address > prev_end)
        vm_log("[vm]   free %llx..%llx (%llu MB, to the limit)\n", prev_end, (unsigned long long)vi.max_address,
               (unsigned long long)((vi.max_address - prev_end) >> 20));
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    vm_address_t a = (vm_address_t)lo;
    kern_return_t kr = vm_allocate(mach_task_self(), &a, (vm_size_t)(hi - lo), VM_FLAGS_FIXED);
    if (kr == KERN_SUCCESS) return true;
    vm_log("[vm] vm_allocate(%llx..%llx, FIXED) = %d%s\n", (unsigned long long)lo, (unsigned long long)hi, kr,
           kr == KERN_NO_SPACE ? " (KERN_NO_SPACE)" : kr == KERN_INVALID_ADDRESS ? " (KERN_INVALID_ADDRESS)" : "");
    vm_report();
    return false;
}

// The smallest free gap below 2^39 (guest addresses are 39 bits) that holds the window, 1 GB-aligned so the game's
// own address arithmetic sees the alignment the Switch's regions have. The map is read with vm_region_recurse_64
// rather than asked for with VM_FLAGS_ANYWHERE: that may answer above 2^39, and never the best-fitting gap.
uint64_t plat_place_guest(uint64_t size) {
    const uint64_t align = 1ull << 30, limit = 1ull << 39;
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    bool have = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS;
    uint64_t max = have ? vi.max_address : limit;
    if (max > limit) max = limit;
    uint64_t best = 0, best_size = ~0ull;
    vm_address_t addr = 0;
    uint64_t prev_end = have ? vi.min_address : 0;  // (below min_address nothing is mappable: not a gap)
    for (int n = 0; n < 4096; n++) {
        vm_size_t rsize;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t icnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        bool last = vm_region_recurse_64(mach_task_self(), &addr, &rsize, &depth, (vm_region_recurse_info_t)&info, &icnt) != KERN_SUCCESS;
        uint64_t gap_end = last ? max : (uint64_t)addr < max ? (uint64_t)addr : max;
        uint64_t lo = (prev_end + align - 1) & ~(align - 1);
        if (gap_end > lo && gap_end - lo >= size && gap_end - lo < best_size) best = lo, best_size = gap_end - lo;
        if (last || (uint64_t)addr >= max) break;
        prev_end = addr + rsize;
        addr = prev_end;
    }
    if (best) {
        vm_address_t a = (vm_address_t)best;
        kern_return_t kr = vm_allocate(mach_task_self(), &a, (vm_size_t)size, VM_FLAGS_FIXED);
        if (kr == KERN_SUCCESS) {
            fprintf(stderr, "[vm] guest window %llx..%llx (%llu MB, in a gap of %llu MB)\n", (unsigned long long)best,
                    (unsigned long long)(best + size), (unsigned long long)(size >> 20), (unsigned long long)(best_size >> 20));
            return best;
        }
        vm_log("[vm] vm_allocate(%llx..%llx, FIXED) = %d (the gap found was not free after all)\n", (unsigned long long)best,
               (unsigned long long)(best + size), kr);
    } else
        vm_log("[vm] no free gap of %llu MB below %llx for the guest window\n", (unsigned long long)(size >> 20),
               (unsigned long long)max);
    vm_report();
    return 0;
}

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    vm_address_t a = (vm_address_t)lo;
    return vm_allocate(mach_task_self(), &a, (vm_size_t)(hi - lo), VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS;
}

void plat_exec(char *const argv[]) { (void)argv; }  // (no exec on iOS: the caller reports the failure)

// A fatal error: what happened in plain words (runtime/crash_explain.c), in SDL's box before the app goes. Only the
// main thread can show it, and it may be waiting for the thread that failed: given 2 s to take the box up, else the
// app ends without it.
static atomic_int g_fatal_box;  // 1: on screen, 2: closed
static void SDLCALL fatal_box(void *text) {
    atomic_store(&g_fatal_box, 1);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LAS Recomp", text, NULL);
    atomic_store(&g_fatal_box, 2);
}
void plat_fatal_notice(void) {
    static char text[1600];
    rt_crash_explain(text, sizeof text, g_log_path);
    if (!g_log_path[0]) return;
    fflush(stderr);
    if (!SDL_RunOnMainThread(fatal_box, text, false)) return;
    for (int ms = 0; atomic_load(&g_fatal_box) < 2; ms += 10) {
        if (ms >= 2000 && !atomic_load(&g_fatal_box)) break;
        usleep(10000);
    }
}

// (child processes: runtime/platform/ios_python.m - Python scripts as threads of the embedded interpreter)
