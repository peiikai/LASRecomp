// The self-updater's core (runtime/update_core.h): the signed manifest, the files it may write, the swap, its journal
// and the rollback, including a swap killed halfway and a folder that can't be written. No game, no network.
// Build + run (from the repo root):
//   c++ -std=c++17 -O1 -Iruntime tools/test_update.cpp -o build/test-update && build/test-update
#include <stdio.h>
#include <stdlib.h>

#include "update_core.h"
// (Monocypher's C compiled into this file as C++, so one command builds it on every platform)
#include "../third_party/monocypher/monocypher.c"
#undef FOR  // (each file defines its own)
#include "../third_party/monocypher/monocypher-ed25519.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t g_sk[64], g_pk[32], g_other_pk[32];
static std::string sign(const std::string &t) {
    uint8_t sig[64];
    crypto_ed25519_sign(sig, g_sk, (const uint8_t *)t.data(), t.size());
    std::string s = t + "signature ";
    char b[3];
    for (uint8_t c : sig) snprintf(b, 3, "%02x", c), s += b;
    return s + "\n";
}
static std::string sha_of(const std::string &data) {
    CacheSha256 h;
    h.add(data.data(), data.size());
    return h.finish();
}
static void write(const std::string &p, const std::string &data) {
    upd_mkdirs_for(p);
    FILE *f = fopen(p.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}
static std::string read(const std::string &p) {
    std::string s;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return "<none>";
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static void manifest_tests() {
    UpdManifest m;
    m.version = "nightly-202611161000-abc1234", m.platform = "windows";
    m.asset = "LAS.Recomp-nightly-202611161000-abc1234-windows.zip", m.asset_size = 5, m.asset_sha256 = sha_of("hello");
    m.files = {{"lasrecomp.exe", 3, sha_of("new")}, {"zig/lib/a b.h", 1, sha_of("x")}};
    std::string good = sign(upd_manifest_text(m)), why;
    UpdManifest r;
    CHECK(upd_parse_manifest(good, g_pk, &r, &why), "good manifest: %s", why.c_str());
    CHECK(r.version == m.version && r.platform == "windows" && r.asset == m.asset && r.asset_size == 5 &&
          r.files.size() == 2 && r.files[1].path == "zig/lib/a b.h", "fields read back");
    CHECK(!upd_parse_manifest(good, g_other_pk, &r, &why) && why == "signature does not match", "another key: %s", why.c_str());
    std::string t = good;
    t[t.find("windows")] = 'W';
    CHECK(!upd_parse_manifest(t, g_pk, &r, &why), "a changed byte");
    t = good;
    t[t.size() - 3] = t[t.size() - 3] == '0' ? '1' : '0';
    CHECK(!upd_parse_manifest(t, g_pk, &r, &why), "a changed signature");
    CHECK(!upd_parse_manifest(good.substr(0, good.size() / 2), g_pk, &r, &why), "truncated");
    {   // written through a Windows text stream (CR LF lines): refused, and said so
        std::string crlf;
        for (char c : good) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
        CHECK(!upd_parse_manifest(crlf, g_pk, &r, &why) && why == "CR LF line ends", "CR LF: %s", why.c_str());
    }
    CHECK(!upd_parse_manifest("", g_pk, &r, &why), "empty");
    CHECK(!upd_parse_manifest(good + "file 1 " + sha_of("x") + " extra\n", g_pk, &r, &why), "a line after the signature");
    // signed, but wrong inside: every one refused
    const char *bad[] = {
        "LAS Recomp update 2\nversion v\nplatform p\nasset 1 %s a\n",          // a newer format
        "LAS Recomp update 1\nversion v\nplatform p\n",                         // no asset
        "LAS Recomp update 1\nversion v\nplatform p\nasset 1 %s a/b\n",        // asset with a folder
        "LAS Recomp update 1\nversion v\nplatform p\nasset x %s a\n",          // size not a number
        "LAS Recomp update 1\nversion v\nplatform p\nasset 1 %s a\nwhat 1\n",  // unknown line
        "LAS Recomp update 1\nversion v\nplatform p\nasset 1 %s a\nfile 1 %s ../evil.dll\n",
        "LAS Recomp update 1\nversion v\nplatform p\nasset 1 %s a\nfile 1 %s /etc/passwd\n",
        "LAS Recomp update 1\nversion v\nplatform p\nasset 1 %s a\nfile 1 %s save/system.bin\n",
        "LAS Recomp update 1\nversion v\nplatform p\nasset 1 %s a\nfile 1 %s Larecomp.ini\n",
        "LAS Recomp update 1\nversion v\nplatform p\nasset 1 %s a\nfile 1 %s extracted/exefs/main\n",
        "LAS Recomp update 1\nversion v\nplatform p\nasset 1 %s a\nfile 1 %s lasrecomp.log\n",
        "LAS Recomp update 1\nversion v\nplatform p\nasset 1 %s a\nfile 1 %s a.exe\nfile 1 %s A.EXE\n",  // twice
        "LAS Recomp update 1\r\nversion v\nplatform p\nasset 1 %s a\n",
    };
    std::string h = sha_of("x");
    for (const char *b : bad) {
        char buf[1024];
        snprintf(buf, sizeof buf, b, h.c_str(), h.c_str(), h.c_str());
        CHECK(!upd_parse_manifest(sign(buf), g_pk, &r, &why), "refused: %s", buf);
    }
    // paths
    const char *ok_paths[] = {"lasrecomp.exe", "zig/lib/std/a.zig", "lasgen/_internal/x.pyd", "README.md", "saves.txt"};
    for (const char *p : ok_paths) CHECK(upd_safe_path(p), "app path %s", p);
    const char *no_paths[] = {"", "/abs", "a//b", "a/./b", "a/../b", "C:/x", "a\\b", "dir/", "rando/x", "SAVE/x",
                              "cache/pipelines", "fonts/a.ttf", "data_root.txt", "lasrecomp-backup-1.log", ".lasrecomp-update/x",
                              "gamecontrollerdb.txt", "x.", "x "};
    for (const char *p : no_paths) CHECK(!upd_safe_path(p), "not an app path: '%s'", p);
}

// An app folder with old files, data next to them (Windows: the data folder is the app's), new files staged
static std::string g_root;
static UpdPaths make_app(const char *name, std::vector<UpdFile> *files) {
    UpdPaths p{g_root + "/" + name};
    upd_remove_tree(p.app);
    write(p.app + "/lasrecomp.exe", "old exe");
    write(p.app + "/SDL3.dll", "old sdl");
    write(p.app + "/zig/lib/a.h", "old a");
    write(p.app + "/save/system.bin", "SAVE");          // data: never touched
    write(p.app + "/larecomp.ini", "settings");
    write(p.app + "/extracted/exefs/main", "game");
    write(p.app + "/old-only.dll", "a file the new version doesn't list");
    *files = {{"lasrecomp.exe", 7, sha_of("new exe")}, {"SDL3.dll", 7, sha_of("new sdl")},
              {"zig/lib/a.h", 5, sha_of("new a")}, {"zig/lib/added.h", 9, sha_of("new added")}};
    write(p.new_dir() + "/lasrecomp.exe", "new exe");
    write(p.new_dir() + "/SDL3.dll", "new sdl");
    write(p.new_dir() + "/zig/lib/a.h", "new a");
    write(p.new_dir() + "/zig/lib/added.h", "new added");
    return p;
}
static bool is_old(const UpdPaths &p) {
    return read(p.app + "/lasrecomp.exe") == "old exe" && read(p.app + "/SDL3.dll") == "old sdl" &&
           read(p.app + "/zig/lib/a.h") == "old a" && !upd_exists(p.app + "/zig/lib/added.h");
}
static bool is_new(const UpdPaths &p) {
    return read(p.app + "/lasrecomp.exe") == "new exe" && read(p.app + "/SDL3.dll") == "new sdl" &&
           read(p.app + "/zig/lib/a.h") == "new a" && read(p.app + "/zig/lib/added.h") == "new added";
}
static bool data_intact(const UpdPaths &p) {
    return read(p.app + "/save/system.bin") == "SAVE" && read(p.app + "/larecomp.ini") == "settings" &&
           read(p.app + "/extracted/exefs/main") == "game" && upd_exists(p.app + "/old-only.dll");
}

static void swap_tests() {
    std::vector<UpdFile> files;
    std::string why;
    {   // the normal way: swap, first start, start screen
        UpdPaths p = make_app("normal", &files);
        CHECK(upd_check_staged(p.new_dir(), files, &why), "staged: %s", why.c_str());
        CHECK(upd_swap(p, files, "v2", "v1", &why), "swap: %s", why.c_str());
        CHECK(is_new(p) && data_intact(p), "new files in place, data untouched");
        CHECK(read(p.old_dir() + "/lasrecomp.exe") == "old exe", "old kept");
        CHECK(upd_boot(p, "v2") == UPD_BOOT_STARTED, "first start");
        CHECK(is_new(p), "still new");
        upd_commit(p);
        CHECK(is_new(p) && data_intact(p) && !upd_exists(p.work()), "committed: work folder gone");
        CHECK(upd_boot(p, "v2") == UPD_BOOT_NOTHING, "later starts: nothing");
    }
    {   // the new version starts but never reaches the start screen: the next start restores
        UpdPaths p = make_app("crashy", &files);
        CHECK(upd_swap(p, files, "v2", "v1", &why), "swap: %s", why.c_str());
        CHECK(upd_boot(p, "v2") == UPD_BOOT_STARTED, "first start");
        CHECK(upd_boot(p, "v2") == UPD_BOOT_RESTORED, "second start without the start screen: restored");
        CHECK(is_old(p) && data_intact(p) && !upd_exists(p.work()), "old files back, data untouched");
    }
    for (int k = 0; k <= 4; k++) {  // killed after k files (k = 4: before the journal said installed)
        UpdPaths p = make_app(("killed" + std::to_string(k)).c_str(), &files);
        CHECK(!upd_swap(p, files, "v2", "v1", &why, k), "stopped after %d", k);
        UpdJournal j;
        CHECK(upd_read_journal(p, &j) && j.state == "swapping", "journal says swapping");
        CHECK(upd_boot(p, "v1") == UPD_BOOT_RESTORED, "killed after %d: restored", k);
        CHECK(is_old(p) && data_intact(p) && !upd_exists(p.work()), "killed after %d: old files back", k);
    }
    {   // killed between "moved" written and the move (the journal names a file still in place)
        UpdPaths p = make_app("journaled", &files);
        write(p.state(), "state swapping \nversion v2\nfrom v1\nmoved lasrecomp.exe\n");
        CHECK(upd_boot(p, "v1") == UPD_BOOT_RESTORED && is_old(p) && data_intact(p), "nothing moved: unchanged");
    }
    {   // killed after the move aside, before the new file went in
        UpdPaths p = make_app("halfmove", &files);
        upd_mkdirs_for(p.old_dir() + "/lasrecomp.exe");
        upd_move(p.app + "/lasrecomp.exe", p.old_dir() + "/lasrecomp.exe");
        write(p.state(), "state swapping \nversion v2\nfrom v1\nmoved lasrecomp.exe\n");
        CHECK(upd_boot(p, "v1") == UPD_BOOT_RESTORED && is_old(p), "moved aside only: put back");
    }
    {   // a half-written journal line (killed while writing it)
        UpdPaths p = make_app("halfline", &files);
        CHECK(!upd_swap(p, files, "v2", "v1", &why, 2), "stopped");
        FILE *f = fopen(p.state().c_str(), "ab");
        fputs("moved zig/li", f);
        fclose(f);
        CHECK(upd_boot(p, "v1") == UPD_BOOT_RESTORED && is_old(p) && data_intact(p), "half line: restored");
    }
    {   // a staged file damaged or missing: refused before anything moves
        UpdPaths p = make_app("damaged", &files);
        write(p.new_dir() + "/SDL3.dll", "new sdX");
        CHECK(!upd_check_staged(p.new_dir(), files, &why), "damaged file refused");
        remove((p.new_dir() + "/zig/lib/a.h").c_str());
        CHECK(!upd_check_staged(p.new_dir(), files, &why), "missing file refused");
        CHECK(is_old(p), "nothing changed");
    }
    {   // a download left behind without a journal: cleaned at start
        UpdPaths p = make_app("leftover", &files);
        CHECK(upd_boot(p, "v1") == UPD_BOOT_NOTHING && !upd_exists(p.work()) && is_old(p), "leftover removed");
    }
    {   // installed, but another version runs (put back by hand): the old files go, nothing else changes
        UpdPaths p = make_app("byhand", &files);
        CHECK(upd_swap(p, files, "v2", "v1", &why), "swap");
        CHECK(upd_boot(p, "v1") == UPD_BOOT_NOTHING && !upd_exists(p.work()) && is_new(p), "other version: kept as is");
    }
#ifndef _WIN32
    if (geteuid() != 0) {  // a folder that can't be written: Update is off (Download remains)
        UpdPaths p = make_app("readonly", &files);
        CHECK(upd_dir_writable(p.app), "writable before");
        chmod(p.app.c_str(), 0555);
        CHECK(!upd_dir_writable(p.app), "read-only folder detected");
        CHECK(!upd_swap(p, files, "v2", "v1", &why), "swap into a read-only folder fails");
        chmod(p.app.c_str(), 0755);
        UpdJournal j;
        if (upd_read_journal(p, &j)) upd_restore(p, j);
        CHECK(is_old(p) && data_intact(p), "read-only: unchanged");
    }
#endif
}

int main() {
    uint8_t seed[32], seed2[32], sk2[64];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1), seed2[i] = (uint8_t)(i * 13 + 5);
    crypto_ed25519_key_pair(g_sk, g_pk, seed);
    crypto_ed25519_key_pair(sk2, g_other_pk, seed2);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TEMP");
    g_root = std::string(tmp && *tmp ? tmp : "/tmp") + "/lasrecomp-test-update";
    for (char &c : g_root) if (c == '\\') c = '/';
    upd_remove_tree(g_root);
    upd_mkdir(g_root.c_str());
    manifest_tests();
    swap_tests();
    upd_remove_tree(g_root);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
