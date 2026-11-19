// The active seed's game files built in memory (build.h).
//
// Most files are kept as built. Room shuffle's level texture banks (DeferredFile: ~434 MB of a room seed, a few tens
// of MB each) are built from the game's banks when the game reads them: at load each is built once to learn its size
// and a hash of its bytes, then dropped; a read builds it again (one kept at a time) and checks it against that hash.
#include "build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "seed_build.h"
#include "../../../third_party/yyjson/yyjson.h"

namespace {
std::mutex g_lock;
std::map<std::string, std::string> g_files;  // romfs path -> bytes
std::vector<std::string> g_paths;            // (every built path, both kinds, sorted)

struct Deferred {
    rando::DeferredFile recipe;
    uint64_t size = 0, hash = 0;
};
std::map<std::string, Deferred> g_deferred;
// the game's files for a deferred file's build (rando_build_seed's reader)
RandoGameRead g_read;
void *g_ctx;
// the deferred file built last (one at a time: a level reads its own bank)
std::string g_cached_path, g_cached;

double now_ms() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

uint64_t hash64(const std::string &s) {  // (a quick check that a rebuild made the same bytes)
    uint64_t h = 0x9E3779B97F4A7C15ull ^ s.size();
    size_t i = 0, n = s.size();
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, s.data() + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < n; i++) h = (h ^ (uint8_t)s[i]) * 0x100000001B3ull;
    return h ^ (h >> 29);
}

bool game_read(const std::string &p, std::string &out) {
    if (!g_read) return false;
    size_t n = 0;
    uint8_t *bytes = g_read(g_ctx, p.c_str(), &n);
    if (!bytes) return false;
    out.assign((const char *)bytes, n);
    free(bytes);
    return true;
}

void clear_locked() {
    g_files.clear(), g_paths.clear(), g_deferred.clear();
    g_read = nullptr, g_ctx = nullptr;
    std::string().swap(g_cached), g_cached_path.clear();
}

// the deferred file's bytes in g_cached (built now unless it's the one kept); false: it can't be built
bool deferred_locked(const std::string &path, const Deferred &d) {
    if (g_cached_path == path) return true;
    std::string().swap(g_cached), g_cached_path.clear();
    double t0 = now_ms();
    try {
        g_cached = rando::build_deferred(d.recipe, game_read);
        g_cached.shrink_to_fit();  // (the writer's string can hold more room than the bank: kept a while)
    } catch (const std::exception &e) {
        fprintf(stderr, "[rando] ERROR: %s can't be built again: %s\n", path.c_str(), e.what());
        return false;
    }
    if (g_cached.size() != d.size || hash64(g_cached) != d.hash)
        fprintf(stderr, "[rando] ERROR: %s built again differs from its first build (%zu bytes, %llu at load)\n",
                path.c_str(), g_cached.size(), (unsigned long long)d.size);
    g_cached_path = path;
    fprintf(stderr, "[rando] built %s: %.1f MB in %.0f ms\n", path.c_str(), g_cached.size() / 1048576.0, now_ms() - t0);
    return true;
}
}  // namespace

extern "C" bool rando_build_seed(const char *json, size_t len, RandoGameRead read, RandoGameList list, void *ctx) {
    std::lock_guard<std::mutex> lk(g_lock);
    clear_locked();
    if (!json || !len) return false;
    rando::Decisions d;
    if (!d.load(std::string(json, len))) { fprintf(stderr, "[rando] build: the seed's decisions don't read\n"); return false; }
    // a dungeon room shuffle seed from before its plan was recorded (seed.json "decisions" "room-plan"): its rooms
    // can't be rebuilt - its files come from its folder, as before
    const char *rooms = yyjson_get_str(yyjson_obj_get(d.raw("settings"), "dungeon-room-shuffle"));
    if (rooms && strcmp(rooms, "off") && !d.raw("room-plan")) {
        fprintf(stderr, "[rando] build: dungeon room shuffle without a recorded plan - the seed's files come from its folder\n");
        return false;
    }
    g_read = read, g_ctx = ctx;
    rando::SeedBuild b(game_read, d);
    // LARECOMP_RANDO_DEFER=0: every file kept as built, the write log too (the build before files were built when
    // read: A/B checks of memory and load time)
    const char *defer = getenv("LARECOMP_RANDO_DEFER");
    bool later = !defer || atoi(defer);
    b.keep_writes(!later);  // (each file's last bytes are all the runtime needs)
    b.set_defer(later);
    b.set_game_list([&](const std::string &dir, std::vector<std::string> &names) {
        size_t n = 0;
        char **v = list(ctx, dir.c_str(), &n);
        if (!v) return false;
        for (size_t i = 0; i < n; i++) names.push_back(v[i]), free(v[i]);
        free(v);
        return true;
    });
    double t0 = now_ms();
    for (const rando::BuildStep &st : rando::build_steps()) {
        if (!rando::build_step_runs(st.name, d)) continue;
        try {
            st.run(b);
        } catch (const std::exception &e) {
            fprintf(stderr, "[rando] build: %s: %s - the seed's files come from its folder\n", st.name, e.what());
            clear_locked();
            return false;
        }
    }
    size_t bytes = 0;
    for (const auto &f : b.files()) {
        if (!f.first.compare(0, 6, "exefs/")) continue;  // (the code patches: runtime/rando.c reads the seed's)
        g_files[f.first] = f.second;
        g_paths.push_back(f.first);
        bytes += f.second.size();
    }
    double t1 = now_ms();
    size_t dbytes = 0;
    for (const auto &kv : b.deferred()) {  // (built once now: their sizes for the RomFS, a hash to check rebuilds)
        Deferred df;
        df.recipe = kv.second;
        try {
            std::string built = rando::build_deferred(kv.second, game_read);
            df.size = built.size(), df.hash = hash64(built);
        } catch (const std::exception &e) {
            fprintf(stderr, "[rando] build: %s: %s - the seed's files come from its folder\n", kv.first.c_str(), e.what());
            clear_locked();
            return false;
        }
        dbytes += df.size;
        g_deferred[kv.first] = df;
        g_paths.push_back(kv.first);
    }
    std::sort(g_paths.begin(), g_paths.end());
    fprintf(stderr, "[rando] built the seed's game files: %zu files, %.1f MB in %.0f ms", g_files.size(),
            bytes / 1048576.0, t1 - t0);
    if (!g_deferred.empty())
        fprintf(stderr, "; %zu built when read (%.1f MB, sized in %.0f ms)", g_deferred.size(), dbytes / 1048576.0,
                now_ms() - t1);
    fprintf(stderr, "\n");
    return true;
}

extern "C" void rando_build_clear(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    clear_locked();
}

extern "C" const uint8_t *rando_built_file(const char *path, size_t *size) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_files.find(path);
    if (it == g_files.end()) return nullptr;
    if (size) *size = it->second.size();
    return (const uint8_t *)it->second.data();
}

extern "C" bool rando_built_deferred(const char *path, uint64_t *size) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_deferred.find(path);
    if (it == g_deferred.end()) return false;
    if (size) *size = it->second.size;
    return true;
}

extern "C" bool rando_built_has(const char *path) {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_files.count(path) || g_deferred.count(path);
}

extern "C" size_t rando_built_read(const char *path, uint64_t off, uint8_t *dst, size_t len) {
    std::lock_guard<std::mutex> lk(g_lock);
    const std::string *src = nullptr;
    auto f = g_files.find(path);
    if (f != g_files.end()) {
        src = &f->second;
    } else {
        auto d = g_deferred.find(path);
        if (d == g_deferred.end() || !deferred_locked(path, d->second)) return 0;
        src = &g_cached;
    }
    if (off >= src->size()) return 0;
    size_t n = len < src->size() - off ? len : (size_t)(src->size() - off);
    memcpy(dst, src->data() + off, n);
    return n;
}

extern "C" size_t rando_built_count(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_paths.size();
}

extern "C" const char *rando_built_path(size_t i) {
    std::lock_guard<std::mutex> lk(g_lock);
    return i < g_paths.size() ? g_paths[i].c_str() : nullptr;
}
