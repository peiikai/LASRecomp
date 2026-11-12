// macOS host layer (runtime/platform.h).
#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "../paths.h"
#include "../platform.h"
#include "darwin_common.h"
#include "posix_log.h"

void rt_crash_explain(char *out, size_t n, const char *log_path);  // runtime/crash_explain.c

// Started as the app (Finder, Dock: no terminal): stdout / stderr go to lasrecomp.log in the data folder, with five
// backups (posix_log.h; as on Linux / Windows), and a fatal error is explained in an alert instead of the app
// vanishing. Runs from a terminal or a test script (not the .app's executable) keep their own output.
static char g_log_path[1024];

static void log_to_file_in_app(void) {
    char exe[1024];
    uint32_t sz = sizeof exe;
    if (isatty(STDERR_FILENO) || _NSGetExecutablePath(exe, &sz) != 0 || !strstr(exe, ".app/Contents/MacOS/")) return;
    char dir[900];
    if (!rt_data_root_early(dir, sizeof dir)) return;  // (runtime/paths.h: the data folder, wherever it was moved)
    mkdir(dir, 0755);
    session_log_open(dir, g_log_path, sizeof g_log_path);
}

void plat_system_info(PlatSysInfo *si) {
    char ver[32], machine[32] = "";
    sysctl_str("kern.osproductversion", ver, sizeof ver);
    struct utsname u;
    if (uname(&u) == 0) snprintf(machine, sizeof machine, "%s", u.machine);
    bool rosetta = sysctl_int("sysctl.proc_translated") == 1;
    snprintf(si->os, sizeof si->os, "macOS %s (%s%s)", ver[0] ? ver : "?", machine, rosetta ? ", Rosetta" : "");
    sysctl_str("machdep.cpu.brand_string", si->cpu, sizeof si->cpu);
    system_info_cores_ram(si);
    char exe[1024];
    uint32_t sz = sizeof exe;
    si->package = _NSGetExecutablePath(exe, &sz) == 0 && strstr(exe, ".app/Contents/MacOS/") ? ".app" : NULL;
}

void plat_init(void) {
    log_to_file_in_app();
    raise_open_file_limit();
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    // Everything readable/writable: macOS pages are 16K but the guest maps at 4K granularity.
    // Untouched pages cost nothing.
    mach_vm_address_t a = lo;
    return mach_vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED) == KERN_SUCCESS;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    mach_vm_address_t a = lo;
    return mach_vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    if (plat_exe_path(path, sizeof path)) execv(path, *_NSGetArgv());
}

// Fatal error: what happened in plain words (runtime/crash_explain.c) - an alert when running as the app (the
// terminal shows the log otherwise). CFUserNotification: safe from any thread, no run loop needed.
void plat_fatal_notice(void) {
    char text[1600];
    rt_crash_explain(text, sizeof text, g_log_path);
    if (!g_log_path[0]) return;
    fflush(stderr);
    CFStringRef msg = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, NULL, NULL, NULL, CFSTR("LAS Recomp"), msg,
                                   CFSTR("OK"), NULL, NULL, NULL);
    if (msg) CFRelease(msg);
}

// ---- child processes
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? PLAT_PROC_SIGNALED + WTERMSIG(st) : 255;
}
