// The self-updater's core (runtime/update.cpp drives it; tools/test_update.cpp tests it without network or game):
// the signed update manifest, the list of app files it may replace, and the journaled swap with its rollback.
//
// Manifest: one per platform and release or nightly round, published next to the file as update-<platform>.txt by
// tools/update_sign (release.yml, nightly.yml). Plain text, LF lines:
//   LAS Recomp update 1
//   version <tag>                       (v0.7.0, nightly-202611161000-abc1234)
//   platform <platform>                 (LARECOMP_PLATFORM: windows, windows-arm64, linux-x86_64, ...)
//   asset <size> <sha256> <file name>   (the file on the release page)
//   file <size> <sha256> <path>         (zips: each app file inside, relative to the zip's top folder; none for AppImages)
//   signature <ed25519, 128 hex>        (over every byte before this line; the key: UPDATE_PUBLIC_KEY in update.cpp)
//
// Swap: the new files are staged in <app>/.lasrecomp-update/new/, then each one is moved into place, the file it
// replaces moved to old/ first. Every step is written to the journal (state) before it happens, so a swap killed
// halfway is undone on the next start. The old files stay until the new version reaches the start screen once; if it
// starts and never gets there, the next start puts them back (upd_boot).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "../third_party/monocypher/monocypher-ed25519.h"
#include "cache_sha256.h"

struct UpdFile {
    std::string path;    // relative, '/' separated
    uint64_t size = 0;
    std::string sha256;  // 64 lowercase hex
};
struct UpdManifest {
    std::string version, platform;
    std::string asset, asset_sha256;
    uint64_t asset_size = 0;
    std::vector<UpdFile> files;
};

static inline bool upd_hex(const std::string &s, uint8_t *out, size_t n) {
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[i * 2 + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;  // (lowercase only)
            if (d < 0) return false;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}
static inline bool upd_is_sha(const std::string &s) {
    uint8_t b[32];
    return upd_hex(s, b, 32);
}
static inline bool upd_parse_u64(const std::string &s, uint64_t *v) {
    if (s.empty() || s.size() > 15) return false;  // (under 1 PB: no overflow)
    *v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        *v = *v * 10 + (uint64_t)(c - '0');
    }
    return true;
}
static inline std::string upd_lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// The update work folder, in the app's folder (removed once the new version reached the start screen)
#define UPD_WORK ".lasrecomp-update"

// A path the updater may write in the app's folder: relative, plain components, never one of the player's data
// entries. On Windows the data folder is the app's folder by default (runtime/paths.h), so a manifest that named a
// save or the settings would overwrite them: refused here even though the manifest is signed (a packaging mistake
// must not cost anyone their saves).
static inline bool upd_safe_path(const std::string &p) {
    if (p.empty() || p.size() > 400 || p[0] == '/' || p.back() == '/') return false;
    for (unsigned char c : p)
        if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            c == 0x7f)
            return false;
    std::string first;
    for (size_t at = 0; at <= p.size();) {
        size_t sl = p.find('/', at);
        if (sl == std::string::npos) sl = p.size();
        std::string c = p.substr(at, sl - at);
        if (c.empty() || c == "." || c == ".." || c.back() == '.' || c.back() == ' ') return false;  // (Windows drops those)
        if (first.empty()) first = upd_lower(c);
        at = sl + 1;
    }
    // runtime/paths.c DATA_ENTRIES and the pointer file, the logs, our own work folder
    static const char *const data[] = {"save", "rando", "larecomp.ini", "fonts", "gamecontrollerdb.txt", "cache",
                                       "extracted", "data_root.txt", UPD_WORK};
    for (const char *d : data)
        if (first == d) return false;
    if (!first.compare(0, 9, "lasrecomp") && first.size() > 4 && !first.compare(first.size() - 4, 4, ".log")) return false;
    return true;
}

// Checks the signature, then reads the manifest. false: why says what's wrong (nothing in m is to be trusted).
static inline bool upd_parse_manifest(const std::string &text, const uint8_t pub[32], UpdManifest *m, std::string *why) {
    *m = UpdManifest();
    auto fail = [&](const char *w) { if (why) *why = w; return false; };
    // (a manifest written through a Windows text stream: every line ends in CR LF, which the signature doesn't cover)
    if (text.find('\r') != std::string::npos) return fail("CR LF line ends");
    size_t sig = text.rfind("\nsignature ");
    if (sig == std::string::npos) return fail("no signature");
    std::string sighex = text.substr(sig + 11);
    if (!sighex.empty() && sighex.back() == '\n') sighex.pop_back();
    uint8_t sb[64];
    if (!upd_hex(sighex, sb, 64)) return fail("bad signature line");
    if (crypto_ed25519_check(sb, pub, (const uint8_t *)text.data(), sig + 1) != 0) return fail("signature does not match");
    std::string body = text.substr(0, sig + 1);
    if (body.find('\r') != std::string::npos || body.find('\0') != std::string::npos) return fail("bad characters");
    std::vector<std::string> lines;
    for (size_t at = 0; at < body.size();) {
        size_t nl = body.find('\n', at);
        lines.push_back(body.substr(at, nl - at));
        at = nl + 1;
    }
    if (lines.empty() || lines[0] != "LAS Recomp update 1") return fail("not an update manifest (or a newer format)");
    // "<size> <sha256> <rest>" (the rest may hold spaces)
    auto sized = [](const std::string &l, uint64_t *size, std::string *sha, std::string *rest) {
        size_t a = l.find(' '), b = a == std::string::npos ? a : l.find(' ', a + 1);
        if (b == std::string::npos) return false;
        *sha = l.substr(a + 1, b - a - 1);
        *rest = l.substr(b + 1);
        return upd_parse_u64(l.substr(0, a), size) && upd_is_sha(*sha) && !rest->empty();
    };
    std::vector<std::string> seen;
    for (size_t i = 1; i < lines.size(); i++) {
        const std::string &l = lines[i];
        if (!l.compare(0, 8, "version ")) m->version = l.substr(8);
        else if (!l.compare(0, 9, "platform ")) m->platform = l.substr(9);
        else if (!l.compare(0, 6, "asset ")) {
            if (!sized(l.substr(6), &m->asset_size, &m->asset_sha256, &m->asset)) return fail("bad asset line");
            if (m->asset.find('/') != std::string::npos || m->asset.find('\\') != std::string::npos) return fail("bad asset name");
        } else if (!l.compare(0, 5, "file ")) {
            UpdFile f;
            if (!sized(l.substr(5), &f.size, &f.sha256, &f.path)) return fail("bad file line");
            if (!upd_safe_path(f.path)) return fail("a file outside the app");
            std::string low = upd_lower(f.path);  // (Windows: one file whatever the case)
            for (const std::string &s : seen)
                if (s == low) return fail("a file listed twice");
            seen.push_back(low);
            m->files.push_back(f);
        } else return fail("unknown line");
    }
    if (m->version.empty() || m->platform.empty() || m->asset.empty()) return fail("version, platform or asset missing");
    return true;
}

// The manifest's text before its signature line (tools/update_sign.c writes it, then signs it)
static inline std::string upd_manifest_text(const UpdManifest &m) {
    std::string t = "LAS Recomp update 1\nversion " + m.version + "\nplatform " + m.platform + "\nasset " +
                    std::to_string(m.asset_size) + " " + m.asset_sha256 + " " + m.asset + "\n";
    for (const UpdFile &f : m.files) t += "file " + std::to_string(f.size) + " " + f.sha256 + " " + f.path + "\n";
    return t;
}

// ---- files
static inline std::string upd_join(const std::string &a, const std::string &b) { return a.empty() ? b : a + "/" + b; }
static inline bool upd_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
static inline bool upd_is_file(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static inline std::string upd_file_sha256(const std::string &p, uint64_t *size) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return "";
    CacheSha256 h;
    static thread_local char buf[1 << 16];
    size_t n;
    uint64_t total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) h.add(buf, n), total += n;
    bool err = ferror(f);
    fclose(f);
    if (err) return "";
    if (size) *size = total;
    return h.finish();
}
static inline int upd_mkdir(const char *d) {
#ifdef _WIN32
    return _mkdir(d);
#else
    return mkdir(d, 0755);
#endif
}
// The folders above path/name (mkdir -p of its parent)
static inline bool upd_mkdirs_for(const std::string &file) {
    for (size_t at = 1; (at = file.find('/', at)) != std::string::npos; at++) {
        std::string d = file.substr(0, at);
        if (d.size() == 2 && d[1] == ':') continue;  // (C:)
        if (upd_mkdir(d.c_str()) != 0 && !upd_exists(d)) return false;
    }
    return true;
}
// rename(), replacing the target (Windows: rename() refuses an existing target; a running .exe or loaded DLL may be
// moved but not deleted, which is why every file being replaced is moved aside, never deleted)
static inline bool upd_move(const std::string &from, const std::string &to) {
#ifdef _WIN32
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}
static inline bool upd_remove_tree(const std::string &p) {
    struct stat st;
#ifdef _WIN32
    if (stat(p.c_str(), &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) { SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL); return DeleteFileA(p.c_str()) != 0; }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((p + "/*").c_str(), &fd);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            ok &= upd_remove_tree(p + "/" + fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(p.c_str()) != 0 && ok;
#else
    if (lstat(p.c_str(), &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return unlink(p.c_str()) == 0;
    bool ok = true;
    if (DIR *d = opendir(p.c_str())) {
        while (struct dirent *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            ok &= upd_remove_tree(p + "/" + e->d_name);
        }
        closedir(d);
    }
    return rmdir(p.c_str()) == 0 && ok;
#endif
}
// The folder accepts new files (a probe file made and removed): false = Update can't work here (Download still does)
static inline bool upd_dir_writable(const std::string &dir) {
    std::string probe = upd_join(dir, ".lasrecomp-write-test");
    FILE *f = fopen(probe.c_str(), "wb");
    if (!f) return false;
    bool ok = fputs("x", f) >= 0;
    ok &= fclose(f) == 0;
    remove(probe.c_str());
    return ok;
}

// A staged copy checked against the manifest: every listed file present with its size and sha256
static inline bool upd_check_staged(const std::string &staged, const std::vector<UpdFile> &files, std::string *why) {
    for (const UpdFile &f : files) {
        uint64_t size = 0;
        std::string sha = upd_file_sha256(upd_join(staged, f.path), &size);
        if (sha.empty()) { if (why) *why = "missing in the download: " + f.path; return false; }
        if (size != f.size || sha != f.sha256) { if (why) *why = "doesn't match the manifest: " + f.path; return false; }
    }
    return true;
}

// ---- the swap and its journal (<app>/.lasrecomp-update/state)
// Lines: "state <swapping|installed|started>", "version <new>", "from <old>", then one line per file before it is
// touched: "moved <path>" (an old file moved to old/<path>) or "added <path>" (none there before). The state line is
// rewritten in place (it's first, and the word is padded to 9 letters) after the journal is synced.
struct UpdPaths {
    std::string app;  // the app's folder
    std::string work() const { return upd_join(app, UPD_WORK); }
    std::string state() const { return upd_join(work(), "state"); }
    std::string old_dir() const { return upd_join(work(), "old"); }
    std::string new_dir() const { return upd_join(work(), "new"); }
};
static inline void upd_sync(FILE *f) {
    fflush(f);
#ifdef _WIN32
    _commit(_fileno(f));
#else
    fsync(fileno(f));
#endif
}
static inline bool upd_set_state(const UpdPaths &p, const char *state) {
    char word[10];
    snprintf(word, sizeof word, "%-9s", state);
    FILE *f = fopen(p.state().c_str(), "r+b");
    if (!f) return false;
    bool ok = fseek(f, 6, SEEK_SET) == 0 && fwrite(word, 1, 9, f) == 9;
    upd_sync(f);
    return fclose(f) == 0 && ok;
}
struct UpdJournal {
    std::string state, version, from;
    std::vector<std::pair<bool, std::string>> entries;  // (moved, path)
};
static inline bool upd_read_journal(const UpdPaths &p, UpdJournal *j) {
    FILE *f = fopen(p.state().c_str(), "rb");
    if (!f) return false;
    *j = UpdJournal();
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        if (!l.empty() && l.back() == '\n') l.pop_back();
        else if (!feof(f)) continue;  // (too long: not ours)
        if (!l.compare(0, 6, "state ")) {
            j->state = l.substr(6);
            while (!j->state.empty() && j->state.back() == ' ') j->state.pop_back();
        }
        else if (!l.compare(0, 8, "version ")) j->version = l.substr(8);
        else if (!l.compare(0, 5, "from ")) j->from = l.substr(5);
        else if (!l.compare(0, 6, "moved ") && upd_safe_path(l.substr(6))) j->entries.push_back({true, l.substr(6)});
        else if (!l.compare(0, 6, "added ") && upd_safe_path(l.substr(6))) j->entries.push_back({false, l.substr(6)});
        // (a half-written last line - killed while writing it - fails upd_safe_path or is ignored: its file wasn't
        //  touched yet)
    }
    fclose(f);
    return !j->state.empty();
}

// Moves the staged files (new/<path>) into the app folder. Test hook: stop_after = n stops (as if killed) after n
// files, journal and all, returning false.
static inline bool upd_swap(const UpdPaths &p, const std::vector<UpdFile> &files, const std::string &version,
                            const std::string &from, std::string *why, int stop_after = -1) {
    FILE *j = fopen(p.state().c_str(), "wb");
    if (!j) { if (why) *why = "can't write the update journal"; return false; }
    fprintf(j, "state %-9s\nversion %s\nfrom %s\n", "swapping", version.c_str(), from.c_str());
    upd_sync(j);
    int done = 0;
    for (const UpdFile &f : files) {
        if (stop_after >= 0 && done == stop_after) { fclose(j); if (why) *why = "stopped (test)"; return false; }
        std::string dst = upd_join(p.app, f.path), src = upd_join(p.new_dir(), f.path), old = upd_join(p.old_dir(), f.path);
        bool had = upd_exists(dst);
        fprintf(j, "%s %s\n", had ? "moved" : "added", f.path.c_str());
        upd_sync(j);
        if (had && !(upd_mkdirs_for(old) && upd_move(dst, old))) {
            fclose(j);
            if (why) *why = "can't move aside: " + f.path;
            return false;
        }
        if (!(upd_mkdirs_for(dst) && upd_move(src, dst))) {
            fclose(j);
            if (why) *why = "can't put in place: " + f.path;
            return false;
        }
        done++;
    }
    fclose(j);
    if (stop_after == done) { if (why) *why = "stopped (test)"; return false; }
    if (!upd_set_state(p, "installed")) { if (why) *why = "can't finish the update journal"; return false; }
    return true;
}

// Puts the old files back (a swap that didn't finish, or a new version that never reached the start screen) and
// removes the work folder. A file in the way (the new one, perhaps running) is moved into trash/ rather than deleted.
static inline bool upd_restore(const UpdPaths &p, const UpdJournal &j) {
    bool ok = true;
    int n = 0;
    std::string trash = upd_join(p.work(), "trash");
    for (auto it = j.entries.rbegin(); it != j.entries.rend(); ++it) {
        std::string dst = upd_join(p.app, it->second), old = upd_join(p.old_dir(), it->second);
        if (it->first && !upd_exists(old)) continue;  // (journaled, never moved: the old file is still in place)
        if (upd_exists(dst)) {
            std::string t = upd_join(trash, std::to_string(n++));
            if (!(upd_mkdirs_for(t) && upd_move(dst, t))) { ok = false; continue; }
        }
        if (it->first && !upd_move(old, dst)) ok = false;
    }
    if (ok) upd_remove_tree(p.work());  // (trash/ may hold a running .exe: removed as far as it can be, next time the rest)
    return ok;
}

// At start, before anything else of the app's is loaded. this_version: LARECOMP_VERSION.
enum { UPD_BOOT_NOTHING, UPD_BOOT_RESTORED, UPD_BOOT_STARTED };
static inline int upd_boot(const UpdPaths &p, const std::string &this_version) {
    UpdJournal j;
    if (!upd_read_journal(p, &j)) {
        if (upd_exists(p.work())) upd_remove_tree(p.work());  // (a download that never got to the swap)
        return UPD_BOOT_NOTHING;
    }
    if (j.state == "swapping" || j.state == "started") return upd_restore(p, j) ? UPD_BOOT_RESTORED : UPD_BOOT_NOTHING;
    if (j.state == "installed" && j.version == this_version) return upd_set_state(p, "started") ? UPD_BOOT_STARTED : UPD_BOOT_NOTHING;
    // installed, but another version runs (put back by hand?): the old files aren't needed by anyone
    upd_remove_tree(p.work());
    return UPD_BOOT_NOTHING;
}
// The new version reached the start screen: the old files go.
static inline void upd_commit(const UpdPaths &p) {
    UpdJournal j;
    if (upd_read_journal(p, &j) && j.state != "started") return;
    upd_remove_tree(p.work());
}
