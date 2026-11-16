// The self-updater's AppImage path end to end (runtime/update.cpp, built with LARECOMP_SELF_UPDATE_TEST), driven by
// tools/test_update_e2e.sh against a local web server: fetch the manifest, check it, download, check, swap, restart,
// first start, keep or roll back. This file stands in for the platform layer (curl as a child process, the exe path).
// Usage: test-update-e2e start <tag> <platform> <min nightly stamp> [restart]  |  boot  |  commit  |  available
//   (TEST_VERSION: the version this "build" is; APPIMAGE: the file it updates)
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform.h"
#include "update.h"

extern char **environ;
extern "C" {
const char *larecomp_version(void) { return getenv("TEST_VERSION") ? getenv("TEST_VERSION") : "v1"; }
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)log_path, (void)append_log;
    pid_t pid;
    return posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) == 0 ? (PlatProc)pid : 0;
}
int plat_proc_wait(PlatProc p, bool block) {
    int st;
    if (waitpid((pid_t)p, &st, block ? 0 : WNOHANG) <= 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : PLAT_PROC_SIGNALED + WTERMSIG(st);
}
void plat_proc_kill(PlatProc p) { kill((pid_t)p, SIGTERM); }
bool plat_exe_path(char *buf, size_t size) { return snprintf(buf, size, "/nonexistent") > 0; }
#ifdef __linux__
void plat_terminate(void) { _exit(0); }
#endif
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "available")) {
        const char *why = "";
        int a = update_available(&why);
        printf("available %d %s\n", a, a ? "" : why);
        return a ? 0 : 1;
    }
    if (!strcmp(cmd, "boot")) { update_boot(); printf("boot: continued\n"); return 0; }
    if (!strcmp(cmd, "commit")) { update_reached_start_screen(); printf("committed\n"); return 0; }
    if (!strcmp(cmd, "start") && argc >= 5) {
        update_start(argv[2], argv[3], atoll(argv[4]));
        char msg[256];
        double frac;
        int st;
        while ((st = update_status(&frac, msg, sizeof msg)) == UPDATE_RUNNING) usleep(20000);
        printf("%s: %s\n", st == UPDATE_DONE ? "done" : "failed", msg);
        fflush(stdout);
        if (st == UPDATE_DONE && argc > 5 && !strcmp(argv[5], "restart")) update_restart();
        return st == UPDATE_DONE ? 0 : 1;
    }
    fprintf(stderr, "usage: see the top of tools/test_update_e2e.cpp\n");
    return 2;
}
