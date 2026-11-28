// Test interposer for tools/test_game_files_read_only.sh (macOS: DYLD_INSERT_LIBRARIES, Linux: LD_PRELOAD): the data
// folder's game files are hard links to the player's own (runtime/romfs_build.c), so LAS Recomp must never write to one.
// Any open for writing, truncate or rename of an existing file under the folders in LARECOMP_WRITE_GUARD (':'-separated,
// absolute, links resolved) is logged to LARECOMP_WRITE_GUARD_LOG (default stderr) as "WRITE-GUARD: ...".
// Creating a new file there and removing a name (unlink) are allowed: that's how setup replaces a link.
//   cc -shared -fPIC -O1 tools/write_guard.c -o build/write_guard.{dylib,so} [-ldl]
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool guarded(const char *path) {
    const char *g = getenv("LARECOMP_WRITE_GUARD");
    struct stat st;
    if (!g || !path || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;  // (new files: fine)
    char real[PATH_MAX];
    if (!realpath(path, real)) return false;
    for (const char *p = g; *p;) {
        const char *e = strchr(p, ':');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && !strncmp(real, p, n) && real[n] == '/') return true;
        p += n + (e != NULL);
    }
    return false;
}

static void report(const char *what, const char *path) {
    int saved = errno;
    const char *l = getenv("LARECOMP_WRITE_GUARD_LOG");
    int fd = l ? -1 : 2;
    static int (*real_open)(const char *, int, ...);
    if (!real_open) real_open = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    if (l) fd = real_open(l, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char b[PATH_MAX + 128];
        int k = snprintf(b, sizeof b, "WRITE-GUARD: %s %s (pid %d)\n", what, path, (int)getpid());
        if (k > 0) (void)!write(fd, b, (size_t)k);
        if (fd != 2) close(fd);
    }
    errno = saved;
}

static bool write_flags(int flags) { return (flags & (O_WRONLY | O_RDWR | O_TRUNC | O_APPEND)) != 0; }
static bool write_mode(const char *m) { return m && (strchr(m, 'w') || strchr(m, 'a') || strchr(m, '+')); }

#ifdef __APPLE__
// (dyld interposing: every image's calls, libc's own included, come here; ours call the real ones)
#define INTERPOSE(rep, orig) \
    __attribute__((used)) static struct { const void *r, *o; } interpose_##orig \
        __attribute__((section("__DATA,__interpose"))) = {(const void *)rep, (const void *)orig}
static int g_open(const char *p, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    if (write_flags(flags) && guarded(p)) report("open for writing", p);
    return open(p, flags, mode);
}
static int g_openat(int dir, const char *p, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    if (write_flags(flags) && (p[0] == '/' || dir == AT_FDCWD) && guarded(p)) report("openat for writing", p);
    return openat(dir, p, flags, mode);
}
static FILE *g_fopen(const char *p, const char *m) {
    if (write_mode(m) && guarded(p)) report("fopen for writing", p);
    return fopen(p, m);
}
static FILE *g_freopen(const char *p, const char *m, FILE *f) {
    if (p && write_mode(m) && guarded(p)) report("freopen for writing", p);
    return freopen(p, m, f);
}
static int g_truncate(const char *p, off_t n) {
    if (guarded(p)) report("truncate", p);
    return truncate(p, n);
}
static int g_rename(const char *a, const char *b) {
    if (guarded(a)) report("rename from", a);
    if (guarded(b)) report("rename onto", b);
    return rename(a, b);
}
INTERPOSE(g_open, open);
INTERPOSE(g_openat, openat);
INTERPOSE(g_fopen, fopen);
INTERPOSE(g_freopen, freopen);
INTERPOSE(g_truncate, truncate);
INTERPOSE(g_rename, rename);
#else
#define REAL(name) \
    static __typeof__(&name) real_##name; \
    if (!real_##name) real_##name = (__typeof__(&name))dlsym(RTLD_NEXT, #name)
int open(const char *p, int flags, ...) {
    REAL(open);
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    if (write_flags(flags) && guarded(p)) report("open for writing", p);
    return real_open(p, flags, mode);
}
int open64(const char *p, int flags, ...) {
    REAL(open64);
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    if (write_flags(flags) && guarded(p)) report("open for writing", p);
    return real_open64(p, flags, mode);
}
int openat(int dir, const char *p, int flags, ...) {
    REAL(openat);
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    if (write_flags(flags) && (p[0] == '/' || dir == AT_FDCWD) && guarded(p)) report("openat for writing", p);
    return real_openat(dir, p, flags, mode);
}
FILE *fopen(const char *p, const char *m) {
    REAL(fopen);
    if (write_mode(m) && guarded(p)) report("fopen for writing", p);
    return real_fopen(p, m);
}
FILE *fopen64(const char *p, const char *m) {
    REAL(fopen64);
    if (write_mode(m) && guarded(p)) report("fopen for writing", p);
    return real_fopen64(p, m);
}
int truncate(const char *p, off_t n) {
    REAL(truncate);
    if (guarded(p)) report("truncate", p);
    return real_truncate(p, n);
}
int rename(const char *a, const char *b) {
    REAL(rename);
    if (guarded(a)) report("rename from", a);
    if (guarded(b)) report("rename onto", b);
    return real_rename(a, b);
}
#endif
