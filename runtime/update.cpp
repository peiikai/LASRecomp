// Self-update for the Windows zip and the Linux AppImage: see update.h (what it does) and update_core.h (the manifest,
// the swap and its journal).
#include "update.h"

#if !LARECOMP_SELF_UPDATE
void update_boot(void) {}
void update_reached_start_screen(void) {}
int update_available(const char **why) { if (why) *why = "not on this platform"; return 0; }
void update_start(const char *, const char *, long long) {}
void update_cancel(void) {}
int update_status(double *frac, char *msg, int n) { if (frac) *frac = -1; if (n > 0) msg[0] = 0; return UPDATE_IDLE; }
void update_restart(void) {}
#else

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>

#include <atomic>
#include <string>

#include "platform.h"
#include "update_core.h"
#ifdef _WIN32
#include "zipfile.h"
#else
#include <sys/stat.h>
#endif

extern "C" const char *larecomp_version(void);  // runtime/ui/launcher.mm (LARECOMP_VERSION)

// The public half of the key that signs every manifest (the UPDATE_SIGNING_KEY secret; tools/update_sign.cpp). The
// same key signs the macOS updates for Sparkle (SUPublicEDKey in tools/bundle_macos.sh, base64 there). Never change
// it: builds out there only accept manifests signed by this one.
#define UPDATE_PUBLIC_KEY "0000000000000000000000000000000000000000000000000000000000000000"
#define DOWNLOAD_URL "https://github.com/peiikai/LASRecomp/releases/download"

// Where the app is: the folder whose files the manifest lists (Windows), or the AppImage's folder and its name
static bool app_location(std::string *dir, std::string *appimage_name, const char **why) {
#ifdef _WIN32
    if (const char *t = getenv("LARECOMP_UPDATE_APP")) { *dir = t; return true; }
#ifndef LARECOMP_ZIP_BUILD
    if (why) *why = "not the release zip (a build from source)";
    return false;
#endif
    char exe[1100];
    if (!plat_exe_path(exe, sizeof exe)) { if (why) *why = "can't find the exe"; return false; }
    std::string e = exe;
    for (char &c : e) if (c == '\\') c = '/';
    size_t sl = e.rfind('/');
    if (sl == std::string::npos) { if (why) *why = "can't find the exe"; return false; }
    *dir = e.substr(0, sl);
    (void)appimage_name;
    return true;
#else
    const char *a = getenv("APPIMAGE");
    if (!a || a[0] != '/') { if (why) *why = "not run as an AppImage"; return false; }
    std::string p = a;
    size_t sl = p.rfind('/');
    *dir = sl == 0 ? "/" : p.substr(0, sl);
    *appimage_name = p.substr(sl + 1);
    return !appimage_name->empty();
#endif
}

static void end_process(void) {
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#else
    _exit(0);  // (the end-to-end test on macOS)
#endif
}

// ---- start and start screen
static void restart_app(const std::string &dir, const std::string &appimage_name);

void update_boot(void) {
    std::string dir, name;
    if (!app_location(&dir, &name, NULL)) return;
    UpdPaths p{dir};
    if (!upd_exists(p.work())) return;
    int r = upd_boot(p, larecomp_version());
    if (r == UPD_BOOT_RESTORED) {
        fprintf(stderr, "[update] the update didn't finish or the new version never reached the start screen: the "
                        "previous version is back\n");
        restart_app(dir, name);
    } else if (r == UPD_BOOT_STARTED) fprintf(stderr, "[update] first start of %s after the update\n", larecomp_version());
}

void update_reached_start_screen(void) {
    static bool once;
    if (once) return;
    once = true;
    std::string dir, name;
    if (!app_location(&dir, &name, NULL)) return;
    UpdPaths p{dir};
    if (upd_exists(p.work())) upd_commit(p);
}

int update_available(const char **why) {
    std::string dir, name;
    if (!app_location(&dir, &name, why)) return 0;
    if (!upd_dir_writable(dir)) { if (why) *why = "the app's folder isn't writable"; return 0; }
#ifndef _WIN32
    if (access((dir + "/" + name).c_str(), W_OK) != 0) { if (why) *why = "the AppImage isn't writable"; return 0; }
#endif
    return 1;
}

// ---- the update itself (one background thread)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_state = UPDATE_IDLE;
static std::string g_msg;
static std::atomic<uint64_t> g_total{0};
static std::string g_part;  // the file being downloaded (its size is the progress)
static std::atomic<bool> g_cancel{false};
static PlatProc g_curl;
static std::string g_dir, g_name;

static void set_status(int st, const std::string &msg) {
    pthread_mutex_lock(&g_lock);
    g_state = st, g_msg = msg;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[update] %s\n", msg.c_str());
}

// url -> path with curl (the update check's: macOS and Windows 10+ ship it; Linux: the system's). false: not there.
static bool fetch(const std::string &url, const std::string &path, int max_secs) {
#ifdef _WIN32
    const char *sr = getenv("SystemRoot");
    std::string curl = std::string(sr ? sr : "C:\\Windows") + "\\System32\\curl.exe";
#else
    std::string curl = "/usr/bin/curl";
#endif
    std::string secs = std::to_string(max_secs), ua = std::string("LASRecomp/") + larecomp_version();
    const char *av[] = {curl.c_str(), "-fsSL", "--retry", "2", "--max-time", secs.c_str(), "-A", ua.c_str(), "-o",
                        path.c_str(), url.c_str(), NULL};
    remove(path.c_str());
    PlatProc pr = plat_spawn(av, NULL, false);
    if (!pr) return false;
    pthread_mutex_lock(&g_lock);
    g_curl = pr;
    pthread_mutex_unlock(&g_lock);
    int rc = plat_proc_wait(pr, true);
    pthread_mutex_lock(&g_lock);
    g_curl = 0;
    pthread_mutex_unlock(&g_lock);
    return rc == 0 && !g_cancel;
}

static bool read_text(const std::string &path, std::string *out, size_t max) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char b[8192];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0 && out->size() <= max) out->append(b, n);
    fclose(f);
    return out->size() <= max;
}

struct Job { std::string tag, platform; long long min_stamp; };

static void *update_thread(void *arg) {
    Job job = *(Job *)arg;
    delete (Job *)arg;
    UpdPaths p{g_dir};
    auto fail = [&](const std::string &why) {
        upd_remove_tree(p.work());
        set_status(UPDATE_FAILED, g_cancel ? std::string("Cancelled.") : why);
        return (void *)NULL;
    };
    upd_remove_tree(p.work());  // (an earlier attempt's leftovers)
    if (upd_mkdir(p.work().c_str()) != 0) return fail("Can't write in the app's folder.");
    const char *base = getenv("LARECOMP_UPDATE_URL");
    std::string url = std::string(base && *base ? base : DOWNLOAD_URL) + "/" + job.tag + "/";

    // the manifest: signed by our key, for this platform, the version that was offered
    set_status(UPDATE_RUNNING, "Checking the update");
    std::string mpath = upd_join(p.work(), "manifest.txt"), text, why;
    if (!fetch(url + "update-" + job.platform + ".txt", mpath, 60) || !read_text(mpath, &text, 1 << 20))
        return fail("Couldn't get the update's details.");
    uint8_t pub[32];
    const char *k = base && *base ? getenv("LARECOMP_UPDATE_KEY") : NULL;  // (tests: their own key, own server)
    upd_hex(k && *k ? k : UPDATE_PUBLIC_KEY, pub, 32);
    UpdManifest m;
    if (!upd_parse_manifest(text, pub, &m, &why)) return fail("The update's details failed the check (" + why + ").");
    if (m.platform != job.platform) return fail("The update is for another platform.");
    if (job.tag == "nightly") {
        long long stamp = 0;  // nightly-<YYYYMMDDHHMM>-<commit>
        if (m.version.compare(0, 8, "nightly-") || sscanf(m.version.c_str() + 8, "%12lld", &stamp) != 1 ||
            stamp <= job.min_stamp)
            return fail("The update isn't newer than this version.");
    } else if (m.version != job.tag) return fail("The update's details are for another version.");
#ifdef _WIN32
    if (m.files.empty()) return fail("The update lists no files.");
#else
    if (!m.files.empty()) return fail("The update isn't an AppImage.");
#endif

    // the file
    set_status(UPDATE_RUNNING, "Downloading");
    std::string dl = upd_join(p.work(), "download");
    pthread_mutex_lock(&g_lock);
    g_part = dl, g_total = m.asset_size;
    pthread_mutex_unlock(&g_lock);
    bool got = fetch(url + m.asset, dl, 3600);
    pthread_mutex_lock(&g_lock);
    g_part.clear();
    pthread_mutex_unlock(&g_lock);
    if (!got) return fail("The download didn't finish.");
    set_status(UPDATE_RUNNING, "Checking the download");
    uint64_t size = 0;
    if (upd_file_sha256(dl, &size) != m.asset_sha256 || size != m.asset_size) return fail("The download is damaged.");

    // staged in new/, every file checked, then swapped
    std::vector<UpdFile> files = m.files;
#ifdef _WIN32
    set_status(UPDATE_RUNNING, "Unpacking");
    char zwhy[256] = "";
    ZipReader *z = zip_open(dl.c_str(), zwhy, sizeof zwhy);
    if (!z) return fail(std::string("Can't open the download (") + zwhy + ").");
    bool ok = zip_extract(z, "LAS Recomp", p.new_dir().c_str(), zwhy, sizeof zwhy, NULL);
    zip_close(z);
    if (!ok) return fail(std::string("Can't unpack the download (") + zwhy + ").");
#else
    upd_mkdir(p.new_dir().c_str());
    if (!upd_move(dl, upd_join(p.new_dir(), g_name)) || chmod(upd_join(p.new_dir(), g_name).c_str(), 0755) != 0)
        return fail("Can't prepare the AppImage.");
    files = {UpdFile{g_name, m.asset_size, m.asset_sha256}};
#endif
    if (!upd_check_staged(p.new_dir(), files, &why)) return fail("The update is damaged (" + why + ").");
    if (g_cancel) return fail("Cancelled.");
    set_status(UPDATE_RUNNING, "Installing");
    if (!upd_swap(p, files, m.version, larecomp_version(), &why)) {
        UpdJournal j;
        if (upd_read_journal(p, &j)) upd_restore(p, j);  // (back to how it was)
        return fail("The update couldn't be installed (" + why + "). Nothing was changed.");
    }
    remove(dl.c_str());
    upd_remove_tree(p.new_dir());
    set_status(UPDATE_DONE, "Installed " + m.version);
    return NULL;
}

void update_start(const char *tag, const char *platform, long long min_nightly_stamp) {
    pthread_mutex_lock(&g_lock);
    bool busy = g_state == UPDATE_RUNNING;
    pthread_mutex_unlock(&g_lock);
    if (busy) return;
    const char *why = NULL;
    if (!update_available(&why)) { set_status(UPDATE_FAILED, std::string("Update isn't possible here: ") + why + "."); return; }
    app_location(&g_dir, &g_name, NULL);
    g_cancel = false;
    set_status(UPDATE_RUNNING, std::string("Updating to ") + tag);
    pthread_t t;
    if (pthread_create(&t, NULL, update_thread, new Job{tag, platform, min_nightly_stamp}) == 0) pthread_detach(t);
    else set_status(UPDATE_FAILED, "Couldn't start the update.");
}

void update_cancel(void) {
    g_cancel = true;
    pthread_mutex_lock(&g_lock);
    if (g_curl) plat_proc_kill(g_curl);
    pthread_mutex_unlock(&g_lock);
}

int update_status(double *frac, char *msg, int n) {
    pthread_mutex_lock(&g_lock);
    int st = g_state;
    if (n > 0) snprintf(msg, (size_t)n, "%s", g_msg.c_str());
    std::string part = g_part;
    pthread_mutex_unlock(&g_lock);
    if (frac) {
        *frac = -1;
        struct stat s;
        if (!part.empty() && g_total && stat(part.c_str(), &s) == 0) *frac = fmin(1.0, (double)s.st_size / (double)g_total);
    }
    return st;
}

static void restart_app(const std::string &dir, const std::string &appimage_name) {
    if (getenv("LARECOMP_UPDATE_NO_RESTART")) { fprintf(stderr, "[update] not restarting (test)\n"); fflush(stderr); end_process(); }
    fflush(stdout), fflush(stderr);
#ifdef _WIN32
    (void)appimage_name;
    char exe[1100];
    std::string path = getenv("LARECOMP_UPDATE_APP") || !plat_exe_path(exe, sizeof exe) ? dir + "/lasrecomp.exe" : exe;
    std::string cmd = "\"" + path + "\"";
    wchar_t wpath[1100], wcmd[1200];
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, 1100);
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wcmd, 1200);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    // (started on its own, not waited for: this process ends now, so nothing keeps the old files open)
    if (CreateProcessW(wpath, wcmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
        end_process();
    }
    fprintf(stderr, "[update] can't start %s (%lu)\n", path.c_str(), GetLastError());
#else
    std::string path = upd_join(dir, appimage_name);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        // (the new AppImage's runtime sets its own; these name the old one's mount)
        unsetenv("APPIMAGE"), unsetenv("APPDIR"), unsetenv("ARGV0");
        if (const char *owd = getenv("OWD")) { if (chdir(owd) != 0) {} }
        unsetenv("OWD");
        execl(path.c_str(), path.c_str(), (char *)NULL);
        _exit(127);
    }
    if (pid > 0) end_process();
    fprintf(stderr, "[update] can't start %s: %s\n", path.c_str(), strerror(errno));
#endif
}

void update_restart(void) {
    pthread_mutex_lock(&g_lock);
    bool done = g_state == UPDATE_DONE;
    pthread_mutex_unlock(&g_lock);
    if (done) restart_app(g_dir, g_name);
}
#endif
