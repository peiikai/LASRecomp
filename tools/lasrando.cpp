// lasrando: the randomizer's command-line tool, built from the same code the game runs (runtime/rando/).
//
//   lasrando convert <seed folder> [<out.json>]   the folder's record as seed.json (an older folder: read from its
//                                                  settings.json + seed_id.txt), to <out.json> or stdout
//   lasrando get <seed folder> <key>...            settings as the runtime reads them: one line per key,
//                                                  "<key> bool=<0|1> int=<n|-> str=<s|->"
//   lasrando dump <romfs> <seed folder | decisions.json> <out folder>
//                                                  the seed's game-file changes, built in memory as the game will
//                                                  (every ported step in the generator's order), written to
//                                                  <out>/romfs and <out>/exefs: what the runtime serves, and
//                                                  the seed's tables (<out>/pots.tsv, ...)
//   lasrando generate <romfs> <request.json> <out folder> [--legacy]
//                                                  a whole seed made natively from the request (solve/generate.h);
//                                                  --legacy: also a seed folder's old files (romfs/, the tables,
//                                                  settings.json, ...)
//   lasrando room-facts <romfs>                   what the room shuffle reads from the game's rooms (solve/rooms.h)
//   lasrando room-cases doors <romfs> <seed folder> <jobs file> [1]
//   lasrando room-cases stairs <romfs> <seed folder>
//                                                  the cases tools/doorjump_test.sh / tools/stairs_test.sh walk in game
//                                                  (every door jump / every stairs and ladder of the seed's dungeons)
//   lasrando strings                              every text the spoiler view and the check tracker can show,
//                                                  one per line: <spoiler|tracker> <composed 0|1> <text>
//                                                  (tools/menu_strings.py, for the translation tables)
//   lasrando serve-check <romfs> <seed folder | decisions.json> <dump folder>
//                                                  the seed built as the runtime builds it (apply/build.h: files built
//                                                  when read), every built file read back in random pieces (and from
//                                                  two threads) and compared with `lasrando dump`'s; load time, first
//                                                  read of each such file and peak memory (LARECOMP_RANDO_DEFER=0:
//                                                  the build that keeps every file)
//
// Build: make lasrando (build/lasrando).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

#include "../runtime/rando/fmt/bfevfl.h"
#include "../runtime/rando/fmt/gsheet.h"
#include "../runtime/rando/fmt/leb.h"
#include "../runtime/rando/fmt/msbt.h"
#include "../runtime/rando/fmt/sarc.h"
#include "../runtime/rando/fmt/lvb.h"
#include "../runtime/rando/mkdirs.h"
#include "../runtime/rando/apply/build.h"
#include "../runtime/rando/apply/seed_build.h"
#include "../runtime/rando/seed.h"
#include "../runtime/rando/solve/outputs.h"
#include "../runtime/rando/solve/prepare.h"
#include "../runtime/rando/solve/entrances.h"
#include "../runtime/rando/solve/generate.h"
#include "../runtime/rando/solve/rooms.h"
#include "../runtime/rando/solve/rooms_lib.h"
#include "../third_party/yyjson/yyjson.h"

static int usage(void) {
    fprintf(stderr, "usage: lasrando convert <seed folder> [<out.json>]\n"
                    "       lasrando get <seed folder> <key>...\n"
                    "       lasrando dump <romfs> <seed folder|decisions.json> <out folder>\n"
                    "       lasrando generate <romfs> <request.json> <out folder> [--legacy]\n"
                    "       lasrando serve-check <romfs> <seed folder|decisions.json> <dump folder>\n"
                    "       lasrando room-facts <romfs>\n"
                    "       lasrando room-cases doors <romfs> <seed folder> <jobs file> [1]\n"
                    "       lasrando room-cases stairs <romfs> <seed folder>\n"
                    "       lasrando strings\n");
    return 2;
}

static void mkdirs(const std::string &path) {  // (the folders of a file's path)
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') rando::make_dir(path.substr(0, i));
}

static bool slurp(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    if (!fseek(f, 0, SEEK_END)) {  // (its size first: one allocation)
        long n = ftell(f);
        if (n > 0) out.reserve((size_t)n);
        fseek(f, 0, SEEK_SET);
    }
    char buf[1 << 16];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    fclose(f);
    return true;
}

static bool list_dir(const std::string &path, std::vector<std::string> &names) {  // (os.listdir)
    DIR *d = opendir(path.c_str());
    if (!d) return false;
    while (struct dirent *e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    return true;
}

// a seed's decisions: a seed folder's (seed.json "decisions") or a decisions JSON file
static bool read_decisions(const std::string &path, std::string &dj) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        const RandoSeed *s = rando_seed_load(path.c_str());
        size_t len = 0;
        char *j = s ? rando_seed_decisions_json(s, &len) : nullptr;
        if (s) rando_seed_free(s);
        if (!j) { fprintf(stderr, "%s: no seed with decisions there (made before seed.json?)\n", path.c_str()); return false; }
        dj.assign(j, len);
        free(j);
        return true;
    }
    if (!slurp(path, dj)) { fprintf(stderr, "can't read %s\n", path.c_str()); return false; }
    return true;
}

// lasrando dump: the full build, written out
static int dump(const std::string &romfs, const std::string &decisions, const std::string &out) {
    std::string dj;
    if (!read_decisions(decisions, dj)) return 2;
    rando::Decisions d;
    if (!d.load(dj)) { fprintf(stderr, "bad decisions\n"); return 2; }
    rando::SeedBuild b([&](const std::string &p, std::string &o) { return slurp(romfs + "/" + p, o); }, d);
    b.set_game_list([&](const std::string &dir, std::vector<std::string> &names) { return list_dir(romfs + "/" + dir, names); });
    for (const rando::BuildStep &st : rando::build_steps()) {
        if (!rando::build_step_known(st.name)) { fprintf(stderr, "step %s has no place in the order\n", st.name); return 1; }
        if (!rando::build_step_runs(st.name, d)) continue;
        size_t before = b.writes().size();
        try {
            st.run(b);
        } catch (const std::exception &e) {
            fprintf(stderr, "%s: %s\n", st.name, e.what());
            return 1;
        }
        if (getenv("LASRANDO_STEPS") && b.writes().size() > before)
            fprintf(stderr, "  %-60s %zu writes\n", st.name, b.writes().size() - before);
    }
    for (const auto &f : b.files()) {
        std::string dst = out + "/" + (f.first.compare(0, 6, "exefs/") ? "romfs/" + f.first : f.first);
        mkdirs(dst);
        std::ofstream(dst, std::ios::binary) << f.second;
    }
    mkdirs(out + "/x");
    for (const auto &t : b.tables()) std::ofstream(out + "/" + t.first, std::ios::binary) << t.second;  // (seed.json "tables")
    fprintf(stderr, "%zu files, %zu writes\n", b.files().size(), b.writes().size());
    return 0;
}

// lasrando serve-check: the seed built as the runtime builds it (runtime/rando/apply/build.h: files built when read
// kept as recipes), then every built file read back through rando_built_read - whole, in random pieces at random
// places, and from two threads at once - and compared with a `lasrando dump` of the same seed
static std::string g_serve_romfs;
static uint8_t *serve_read(void *, const char *path, size_t *size) {  // (as fs.c's: one malloc'd copy)
    FILE *f = fopen((g_serve_romfs + "/" + path).c_str(), "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *m = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    size_t got = n > 0 ? fread(m, 1, (size_t)n, f) : 0;
    fclose(f);
    *size = got;
    return m;
}
static char **serve_list(void *, const char *dir, size_t *count) {
    std::vector<std::string> names;
    if (!list_dir(g_serve_romfs + "/" + dir, names)) return nullptr;
    char **v = (char **)calloc(names.size() + 1, sizeof *v);
    for (size_t i = 0; i < names.size(); i++) v[i] = strdup(names[i].c_str());
    *count = names.size();
    return v;
}
static double peak_mb() {
#ifndef _WIN32
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / 1048576.0;
#else
    return ru.ru_maxrss / 1024.0;
#endif
#else
    return 0;
#endif
}
static double now_mb() {  // the data the process holds now (macOS: allocated bytes; Linux: resident)
#ifdef __APPLE__
    // (not the footprint: freed big blocks the allocator keeps for reuse stay in it)
    malloc_statistics_t st;
    malloc_zone_statistics(nullptr, &st);
    return st.size_in_use / 1048576.0;
#elif defined(__linux__)
    long pages = 0, res = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f && fscanf(f, "%ld %ld", &pages, &res) == 2) { fclose(f); return res * (sysconf(_SC_PAGESIZE) / 1048576.0); }
    if (f) fclose(f);
    return 0;
#else
    return 0;
#endif
}
static int serve_check(const std::string &romfs, const std::string &decisions, const std::string &dumped) {
    g_serve_romfs = romfs;
    std::string dj;
    if (!read_decisions(decisions, dj)) return 2;
    auto t0 = std::chrono::steady_clock::now();
    if (!rando_build_seed(dj.data(), dj.size(), serve_read, serve_list, nullptr)) { fprintf(stderr, "build failed\n"); return 1; }
    double load = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("load: %.2f s, peak %.0f MB, holding %.0f MB\n", load, peak_mb(), now_mb());
    std::mt19937 rng(12345);
    int bad = 0;
    size_t nfiles = rando_built_count(), ndeferred = 0;
    std::vector<std::string> deferred;
    for (size_t i = 0; i < nfiles; i++) {
        std::string p = rando_built_path(i), want;
        if (!slurp(dumped + "/romfs/" + p, want)) { printf("%s: not in the dump\n", p.c_str()); bad++; continue; }
        uint64_t size = 0;
        bool later = rando_built_deferred(p.c_str(), &size);
        if (!later) {
            size_t n = 0;
            const uint8_t *m = rando_built_file(p.c_str(), &n);
            if (!m || std::string((const char *)m, n) != want) printf("%s: differs\n", p.c_str()), bad++;
            continue;
        }
        ndeferred++, deferred.push_back(p);
        if (size != want.size()) { printf("%s: size %llu, the dump's %zu\n", p.c_str(), (unsigned long long)size, want.size()); bad++; }
        auto t1 = std::chrono::steady_clock::now();
        std::string got(want.size(), '\0');
        size_t at = 0;
        while (at < got.size()) {  // whole, in random pieces
            size_t len = 1 + rng() % (1 << 20);
            size_t r = rando_built_read(p.c_str(), at, (uint8_t *)&got[at], len);
            if (!r) break;
            at += r;
        }
        double first = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        if (got != want) printf("%s: read back differs\n", p.c_str()), bad++;
        for (int k = 0; k < 2000; k++) {  // random places, past the end too
            uint64_t off = rng() % (want.size() + 64);
            size_t len = 1 + rng() % 70000;
            std::string buf(len, '\x55');
            size_t r = rando_built_read(p.c_str(), off, (uint8_t *)&buf[0], len);
            size_t expect = off >= want.size() ? 0 : std::min<size_t>(len, want.size() - off);
            if (r != expect || buf.compare(0, r, want, (size_t)std::min<uint64_t>(off, want.size()), r)) {
                printf("%s: read at %llu (%zu) differs\n", p.c_str(), (unsigned long long)off, len), bad++;
                break;
            }
        }
        double mb = want.size() / 1048576.0;
        std::string().swap(want), std::string().swap(got);  // (the check's own copies: not the build's)
        printf("%s: %.1f MB, first read %.2f s, holding %.0f MB\n", p.c_str(), mb, first, now_mb());
    }
    if (deferred.size() >= 2) {  // two threads on two banks at once (the cache switching between them)
        std::vector<std::string> w(2);
        for (int k = 0; k < 2; k++) slurp(dumped + "/romfs/" + deferred[k], w[k]);
        int tbad[2] = {0, 0};
        std::vector<std::thread> th;
        for (int k = 0; k < 2; k++)
            th.emplace_back([&, k] {
                std::mt19937 r2(k + 1);
                for (int j = 0; j < 40; j++) {
                    uint64_t off = r2() % w[k].size();
                    size_t len = 1 + r2() % 200000;
                    std::string buf(len, '\0');
                    size_t got = rando_built_read(deferred[k].c_str(), off, (uint8_t *)&buf[0], len);
                    size_t expect = std::min<size_t>(len, w[k].size() - off);
                    if (got != expect || buf.compare(0, got, w[k], off, got)) tbad[k]++;
                }
            });
        for (auto &t : th) t.join();
        printf("two threads: %d bad reads\n", tbad[0] + tbad[1]);
        bad += tbad[0] + tbad[1];
    }
    printf("%zu built files (%zu built when read): %s; peak %.0f MB, holding %.0f MB\n", nfiles, ndeferred,
           bad ? "DIFFERENT" : "all the same", peak_mb(), now_mb());
    rando_build_clear();
    printf("after clear: holding %.0f MB\n", now_mb());
    return bad ? 1 : 0;
}

// lasrando room-cases: the in-game room shuffle tests' cases from a seed (tools/doorjump_test.sh, tools/stairs_test.sh)

// a file of the seed at a romfs path: built from its decisions (a seed made by the app), else its folder's romfs/ (an
// older seed), else the game's
static bool seed_file(const std::string &seed_dir, const std::string &romfs, const std::string &path, std::string &out) {
    uint64_t size = 0;
    if (rando_built_deferred(path.c_str(), &size)) {
        out.assign((size_t)size, '\0');
        return rando_built_read(path.c_str(), 0, (uint8_t *)&out[0], (size_t)size) == size;
    }
    size_t n = 0;
    if (const uint8_t *m = rando_built_file(path.c_str(), &n)) return out.assign((const char *)m, n), true;
    return slurp(seed_dir + "/romfs/" + path, out) || slurp(romfs + "/" + path, out);
}

static const RandoSeed *room_cases_seed(const std::string &romfs, const std::string &seed_dir) {
    const RandoSeed *s = rando_seed_load(seed_dir.c_str());
    if (!s) { fprintf(stderr, "%s: no seed there\n", seed_dir.c_str()); return nullptr; }
    size_t len = 0;
    if (char *dj = rando_seed_decisions_json(s, &len)) {
        g_serve_romfs = romfs;
        bool ok = rando_build_seed(dj, len, serve_read, serve_list, nullptr);
        free(dj);
        if (!ok) { fprintf(stderr, "%s: the seed can't be built\n", seed_dir.c_str()); rando_seed_free(s); return nullptr; }
    }
    return s;
}

static std::vector<std::vector<std::string>> seed_rows(const RandoSeed *s, const char *table) {
    std::vector<std::vector<std::string>> rows;
    RandoText t;
    char line[4096];
    if (!rando_seed_text(s, &t, table)) return rows;
    while (rando_text_gets(line, sizeof line, &t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.find_first_not_of(" \t") == std::string::npos) continue;
        std::vector<std::string> cols;
        for (size_t a = 0, b; ; a = b + 1) {
            b = l.find('\t', a);
            cols.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
        }
        rows.push_back(cols);
    }
    return rows;
}

static std::string gfmt(double v) {  // Python's '%g'
    char b[64];
    snprintf(b, sizeof b, "%g", v);
    return b;
}

// lasrando room-cases doors <romfs> <seed folder> <jobs file> [1]: every doorway of the seed's door jumps (level, cell,
// side, slot, the cell Link must end up in, the floor in the doorway) to <jobs file>; and, on stdout, the walls whose
// several doorways must all lead to one place (a single-screen room's wall), checked without the game. 1: only those
// walls' doorways go to the jobs file (one-way doors Link can't leave through left out).
static int room_cases_doors(const std::string &romfs, const std::string &seed_dir, const std::string &jobs_path,
                            bool only_groups) {
    namespace R = rando::solve::rooms;
    const RandoSeed *s = room_cases_seed(romfs, seed_dir);
    if (!s) return 2;
    std::vector<std::vector<std::string>> rows = seed_rows(s, "door_jumps");
    std::set<std::string> levels;
    for (const auto &r : seed_rows(s, "dungeon_map"))
        if (r.size() > 1) levels.insert(r[0]);
    auto neighbour = [](const std::string &cell, const std::string &side) {
        std::pair<int, int> p = R::cell_pos("x_" + cell);
        int i = R::side_index(side[0]);
        return R::cell_name("x", p.first + R::STEP_C[i], p.second + R::STEP_R[i]).substr(2);
    };
    auto target = [&](const std::string &lv, const std::string &cell, const std::string &side, int slot) {
        for (const auto &w : rows)
            if (w.size() > 3 && w[0] == lv && w[1] == cell && w[2] == side && atoi(w[3].c_str()) == slot)
                return w.size() > 6 ? std::make_pair(w[5], w[6])
                                    : std::make_pair(neighbour(cell, side), std::string(1, R::SIDES[R::OPP[R::side_index(side[0])]]));
        return std::make_pair(neighbour(cell, side), std::string(1, R::SIDES[R::OPP[R::side_index(side[0])]]));
    };
    struct Member { std::string cell, side; int slot; };
    std::vector<std::pair<std::string, std::vector<Member>>> groups;
    for (const std::string &lv : levels) {  // (one cell's side with several doorways that jump: Chaos / Madness)
        std::map<std::pair<std::string, std::string>, std::set<int>> sides;
        for (const auto &w : rows)
            if (w.size() > 3 && w[0] == lv) sides[{w[1], w[2]}].insert(atoi(w[3].c_str()));
        for (const auto &e : sides)
            if (e.second.size() > 1) {
                std::vector<Member> m;
                for (int x : e.second) m.push_back({e.first.first, e.first.second, x});
                groups.push_back({lv, m});
            }
    }
    int bad = 0;
    for (const auto &g : groups) {
        std::set<std::pair<std::string, std::string>> ts;
        std::string ms;
        for (const Member &m : g.second) {
            ts.insert(target(g.first, m.cell, m.side, m.slot));
            ms += (ms.empty() ? "" : " ") + m.cell + m.side + std::to_string(m.slot);
        }
        std::string tl;
        for (const auto &t : ts) tl += (tl.empty() ? "" : " / ") + t.first + " " + t.second;
        bad += ts.size() != 1;
        printf("[group] %s %s -> %s: %s\n", g.first.c_str(), ms.c_str(), tl.c_str(), ts.size() == 1 ? "OK" : "GROUP FAIL");
    }
    printf("[group] %zu walls with several doorways, %d lead to more than one place\n", groups.size(), bad);

    // the floor in the doorway in the seed's room (raised doorways: 1.5): the first walkable tile from the edge inward
    auto floor = [&](const std::string &lv, const std::string &cell, const std::string &side, int slot) -> double {
        std::string data;
        if (!seed_file(seed_dir, romfs, "region_common/level/" + lv + "/" + lv + "_" + cell + ".leb", data)) return 0.0;
        rando::LebRoom room = rando::LebRoom::parse(data, true);
        if (!room.has_grid) return 0.0;
        int a = slot / 2;
        for (int i = 0; i < 4; i++) {
            int r, c;
            switch (side[0]) {
            case 'N': r = i, c = a; break;
            case 'S': r = 7 - i, c = a; break;
            case 'W': r = std::min(a, 7), c = i; break;
            default: r = std::min(a, 7), c = 9 - i; break;
            }
            size_t k = (size_t)(std::min(r, 7) * 10 + std::min(c, 9));
            if (k >= room.grid.tiles.size()) return 0.0;
            const rando::LebTile &t = room.grid.tiles[k];
            if (!((t.flags[0] & 2) && !(t.flags[2] & 8))) return t.elevation > 0.3 ? (double)(float)t.elevation : 0.0;
        }
        return 0.0;
    };
    FILE *f = fopen(jobs_path.c_str(), "w");
    if (!f) { fprintf(stderr, "can't write %s\n", jobs_path.c_str()); rando_seed_free(s); return 2; }
    if (only_groups) {
        const R::Data &cat = R::data();
        std::set<std::tuple<std::string, std::string, std::string, int>> seen;
        for (const auto &g : groups)
            for (const Member &m : g.second) {
                const R::Room *room = cat.room(g.first + "_" + m.cell);  // (the walls' rooms stay home)
                bool no_exit = false;
                if (room)
                    for (const R::Door &d : room->doors) {  // (room_logic.door_terms on own_doors, leaving)
                        if (d.side != R::side_index(m.side[0]) || std::abs(d.half - m.slot) > 1 || d.mirror) continue;
                        if (d.kind == "blocked:statue" || d.kind == "blocked:chest") no_exit = true;
                        if (d.kind == "oneway" &&
                            std::find(room->types.begin(), room->types.end(), 0x1A3) == room->types.end())
                            no_exit = true;
                    }
                if (no_exit) {
                    printf("[group] %s %s%s%d: a one-way door - no way out here (not walked)\n", g.first.c_str(),
                           m.cell.c_str(), m.side.c_str(), m.slot);
                    continue;
                }
                if (seen.insert({g.first, m.cell, m.side, m.slot}).second)
                    fprintf(f, "%s\t%s\t%s\t%d\t%s\t%s\n", g.first.c_str(), m.cell.c_str(), m.side.c_str(), m.slot,
                            target(g.first, m.cell, m.side, m.slot).first.c_str(), gfmt(floor(g.first, m.cell, m.side, m.slot)).c_str());
            }
    } else {
        for (const auto &w : rows) {
            if (w.size() < 4) continue;
            fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n", w[0].c_str(), w[1].c_str(), w[2].c_str(), w[3].c_str(),
                    (w.size() > 6 ? w[5] : neighbour(w[1], w[2])).c_str(),
                    gfmt(floor(w[0], w[1], w[2], atoi(w[3].c_str()))).c_str());
        }
    }
    fclose(f);
    rando_seed_free(s);
    return 0;
}

// lasrando room-cases stairs <romfs> <seed folder>: every stairs / ladder transition of the seed's generated dungeons,
// one row per AreaUpstairs (a side-view passage's ladder top) and AreaDownstairs (a room's stairs down):
//   level  kind(up/down)  from room  trigger x y z  target locator  target area x0 z0 x1 z1  gate flag  mode
// (gate flag: the event flag the trigger waits for - stairs that appear once a room is cleared - or -1; mode: how Link
// goes, e.g. LadderUp / DefaultDownStairs). The target area is the room named after the target locator.
static int room_cases_stairs(const std::string &romfs, const std::string &seed_dir) {
    const RandoSeed *s = room_cases_seed(romfs, seed_dir);
    if (!s) return 2;
    std::vector<std::string> levels;
    for (const auto &r : seed_rows(s, "dungeon_map"))
        if (!r.empty() && std::find(levels.begin(), levels.end(), r[0]) == levels.end()) levels.push_back(r[0]);
    auto str = [](const rando::LebParam &p) { return p.kind == rando::LebParam::Str ? std::string(p.s) : std::to_string(p.i); };
    for (const std::string &lv : levels) {
        std::string lvb;
        if (!seed_file(seed_dir, romfs, "region_common/level/" + lv + "/" + lv + ".lvb", lvb)) {
            fprintf(stderr, "# %s: no level file\n", lv.c_str());
            continue;
        }
        rando::FixedHash fh = rando::FixedHash::parse(lvb);
        std::map<std::string, std::array<float, 6>> areas;  // (sorted, as Python's sorted(areas))
        std::set<std::string> starts;
        if (const rando::FhEntry *e = fh.find("area"))
            for (const rando::FhEntry &a : e->sub->entries) {
                std::array<float, 6> v{};
                for (int k = 0; k < 6; k++) v[k] = rando::read_f32(a.data, 4 * k);
                areas[a.name] = v;
            }
        if (const rando::FhEntry *e = fh.find("tagPlayerStart"))
            for (const rando::FhEntry &a : e->sub->entries) starts.insert(a.name);
        for (const auto &ar : areas) {
            const std::string &room_name = ar.first;
            std::string data;
            if (!seed_file(seed_dir, romfs, "region_common/level/" + lv + "/" + room_name + ".leb", data)) continue;
            rando::LebRoom room = rando::LebRoom::parse(data);
            for (const rando::LebActor &a : room.actors) {
                const char *kind = !a.name.compare(0, 12, "AreaUpstairs") ? "up" : !a.name.compare(0, 14, "AreaDownstairs") ? "down" : nullptr;
                if (!kind) continue;
                std::string tgt = str(a.params[0]);
                if (!starts.count(tgt)) {
                    fprintf(stderr, "# %s %s: %s target %s has no start in the level\n", lv.c_str(), room_name.c_str(), kind, tgt.c_str());
                    continue;
                }
                std::string area = areas.count(tgt) ? tgt : tgt.substr(0, tgt.rfind('_'));
                auto it = areas.find(area);
                if (it == areas.end()) {
                    fprintf(stderr, "# %s %s: %s target %s: no room of that name\n", lv.c_str(), room_name.c_str(), kind, tgt.c_str());
                    continue;
                }
                const std::array<float, 6> &b = it->second;  // (x0, y0, z0, x1, y1, z1)
                int gate = a.switches[0].first == 1 ? a.switches[0].second : -1;  // (switch type 1: a global event flag)
                std::string mode = a.params[1].kind == rando::LebParam::Str ? std::string(a.params[1].s) : "";
                printf("%s\t%s\t%s\t%.2f\t%.2f\t%.2f\t%s\t%s\t%s\t%s\t%s\t%d\t%s\n", lv.c_str(), kind, room_name.c_str(),
                       (double)(float)a.pos[0], (double)(float)a.pos[1], (double)(float)a.pos[2], tgt.c_str(),
                       gfmt(b[0]).c_str(), gfmt(b[2]).c_str(), gfmt(b[3]).c_str(), gfmt(b[5]).c_str(), gate,
                       mode.empty() ? "-" : mode.c_str());
            }
        }
    }
    rando_seed_free(s);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "dump")) return dump(argv[2], argv[3], argv[4]);
    if (argc >= 5 && !strcmp(argv[1], "serve-check")) return serve_check(argv[2], argv[3], argv[4]);
    if (argc >= 5 && !strcmp(argv[1], "generate"))
        return rando::solve::generate_main(argv[2], argv[3], argv[4], argc > 5 && !strcmp(argv[5], "--legacy"));
    if (argc >= 3 && !strcmp(argv[1], "room-facts")) return rando::solve::room_facts(argv[2]);
    if (argc >= 6 && !strcmp(argv[1], "room-cases") && !strcmp(argv[2], "doors"))
        return room_cases_doors(argv[3], argv[4], argv[5], argc > 6 && !strcmp(argv[6], "1"));
    if (argc >= 5 && !strcmp(argv[1], "room-cases") && !strcmp(argv[2], "stairs")) return room_cases_stairs(argv[3], argv[4]);
    if (argc >= 2 && !strcmp(argv[1], "strings")) {
        for (const rando::solve::TextKey &k : rando::solve::text_keys())
            printf("%s\t%d\t%s\n", k.group, k.composed ? 1 : 0, k.text.c_str());
        return 0;
    }
    if (argc < 3) return usage();
    const RandoSeed *s = rando_seed_load(argv[2]);
    if (!s) { fprintf(stderr, "%s: no seed there\n", argv[2]); return 1; }
    if (!strcmp(argv[1], "convert")) {
        size_t n;
        char *j = rando_seed_json(s, &n);
        if (!j) return 1;
        FILE *f = argc > 3 ? fopen(argv[3], "wb") : stdout;
        if (!f) { perror(argv[3]); return 1; }
        fwrite(j, 1, n, f);
        if (f != stdout) fclose(f);
        free(j);
        return 0;
    }
    if (!strcmp(argv[1], "get")) {
        for (int i = 3; i < argc; i++) {
            const char *k = argv[i], *str = rando_seed_str(s, k);
            int64_t v = rando_seed_int(s, k, INT64_MIN);
            printf("%s bool=%d int=", k, rando_seed_bool(s, k));
            if (v == INT64_MIN) printf("-"); else printf("%lld", (long long)v);
            printf(" str=%s\n", str ? str : "-");
        }
        return 0;
    }
    return usage();
}
