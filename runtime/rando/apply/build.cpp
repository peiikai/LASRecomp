// The active seed's game files built in memory (build.h).
#include "build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "seed_build.h"
#include "../../../third_party/yyjson/yyjson.h"

namespace {
std::mutex g_lock;
std::map<std::string, std::string> g_files;  // romfs path -> bytes
std::vector<std::string> g_paths;            // (the keys, sorted)

double now_ms() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
}  // namespace

extern "C" bool rando_build_seed(const char *json, size_t len, RandoGameRead read, RandoGameList list, void *ctx) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_files.clear(), g_paths.clear();
    if (!json || !len) return false;
    rando::Decisions d;
    if (!d.load(std::string(json, len))) { fprintf(stderr, "[rando] build: the seed's decisions don't read\n"); return false; }
    // dungeon room shuffle builds new rooms and texture banks: not in the native build yet (docs/randomizer.md, stage 4)
    const char *rooms = yyjson_get_str(yyjson_obj_get(d.raw("settings"), "dungeon-room-shuffle"));
    if (rooms && strcmp(rooms, "off")) {
        fprintf(stderr, "[rando] build: dungeon room shuffle - the seed's files come from its folder\n");
        return false;
    }
    rando::SeedBuild b(
        [&](const std::string &p, std::string &out) {
            size_t n = 0;
            uint8_t *bytes = read(ctx, p.c_str(), &n);
            if (!bytes) return false;
            out.assign((const char *)bytes, n);
            free(bytes);
            return true;
        },
        d);
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
    fprintf(stderr, "[rando] built the seed's game files: %zu files, %.1f MB, %zu writes in %.0f ms\n", g_files.size(),
            bytes / 1048576.0, b.writes().size(), now_ms() - t0);
    return true;
}

extern "C" void rando_build_clear(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_files.clear(), g_paths.clear();
}

extern "C" const uint8_t *rando_built_file(const char *path, size_t *size) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_files.find(path);
    if (it == g_files.end()) return nullptr;
    if (size) *size = it->second.size();
    return (const uint8_t *)it->second.data();
}

extern "C" size_t rando_built_count(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_paths.size();
}

extern "C" const char *rando_built_path(size_t i) {
    std::lock_guard<std::mutex> lk(g_lock);
    return i < g_paths.size() ? g_paths[i].c_str() : nullptr;
}
