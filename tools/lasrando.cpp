// lasrando: the randomizer's command-line tool, built from the same code the game runs (runtime/rando/).
//
//   lasrando convert <seed folder> [<out.json>]   the folder's record as seed.json (an older folder: read from its
//                                                  settings.json + seed_id.txt), to <out.json> or stdout
//   lasrando get <seed folder> <key>...            settings as the runtime reads them: one line per key,
//                                                  "<key> bool=<0|1> int=<n|-> str=<s|->"
//   lasrando repack <kind> <out folder> < paths     each listed game file read and written back by the format code
//                                                  (kind: leb, leb-grid, lvb, gsheet, msbt, sarc, bfevfl), to <out folder>/<the path as listed>;
//                                                  tools/rando/test_formats.py compares that with the Python's
//   lasrando dump <romfs> <decisions.json> <out folder>
//                                                  the seed's game-file changes, built in memory as the game will
//                                                  (every ported step in the generator's order), written to
//                                                  <out>/romfs and <out>/exefs: what the runtime serves, and
//                                                  the seed's tables (<out>/pots.tsv, ...)
//   lasrando check-steps <romfs> <decisions.json> <record folder>
//                                                  each ported generator step (runtime/rando/apply/) run from the
//                                                  Python's state just before it (tools/rando/record.py's write log)
//                                                  and its writes compared with the Python's, write by write
//   lasrando solve-check <prefill.json> <postfill.json>
//                                                  the native item fill (runtime/rando/solve/) from a recorded run's
//                                                  state before LAS's fill, compared with what the Python made
//                                                  (placements, indexes, random state, settings)
//   lasrando generate <romfs> <request.json> <out folder> [--legacy]
//                                                  a whole seed made natively from the request (solve/generate.h),
//                                                  as tools/rando/las_headless.py makes it; --legacy: also the files
//                                                  the Python writes (romfs/, the tables, settings.json, ...).
//                                                  tools/rando/test_generate.py compares the two
//   lasrando decide-check <romfs> <record folder> <seed folder> [<out>]
//                                                  the seed built in decide mode (runtime/rando/apply/decide.h) from
//                                                  the fill's result, against the Python's: decisions.json (every
//                                                  field), the tables and the files (decide_check.h)
//   lasrando texture-check <python file> <native file> <romfs path>
//                                                  a file whose textures the build compresses itself (the chest looks'
//                                                  models, ui/StartUp.arc): byte-identical apart from the pixel data,
//                                                  the pixels within a small error (fmt/texture_check.h)
//   lasrando serve-check <romfs> <decisions.json> <dump folder>
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
#include "../runtime/rando/fmt/texture_check.h"
#include "../runtime/rando/fmt/lvb.h"
#include "../runtime/rando/mkdirs.h"
#include "../runtime/rando/apply/build.h"
#include "../runtime/rando/apply/decide_check.h"
#include "../runtime/rando/apply/seed_build.h"
#include "../runtime/rando/seed.h"
#include "../runtime/rando/solve/check.h"
#include "../runtime/rando/solve/outputs.h"
#include "../runtime/rando/solve/prepare.h"
#include "../runtime/rando/solve/entrances.h"
#include "../runtime/rando/solve/generate.h"
#include "../third_party/yyjson/yyjson.h"

static int usage(void) {
    fprintf(stderr, "usage: lasrando convert <seed folder> [<out.json>]\n"
                    "       lasrando get <seed folder> <key>...\n"
                    "       lasrando dump <romfs> <decisions.json> <out folder>\n"
                    "       lasrando generate <romfs> <request.json> <out folder> [--legacy]\n"
                    "       lasrando repack <leb|leb-grid|lvb|gsheet|msbt|bfevfl|sarc> <out folder> < paths\n"
                    "       lasrando solve-check <prefill.json> <postfill.json>\n"
                    "       lasrando decide-check <romfs> <record folder> <seed folder> [<out folder>]\n"
                    "       lasrando texture-check <python file> <native file> <romfs path>\n"
                    "       lasrando serve-check <romfs> <decisions.json> <dump folder>\n");
    return 2;
}

static void mkdirs(const std::string &path) {  // (the folders of a file's path)
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') rando::make_dir(path.substr(0, i));
}

// read every listed file, run it through the format code, write the result (or "ERROR <what>" in place of it)
static int repack(const std::string &kind, const std::string &out) {
    int n = 0, bad = 0;
    for (std::string path; std::getline(std::cin, path);) {
        if (path.empty()) continue;
        std::ifstream in(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), res;
        try {
            if (kind == "leb" || kind == "leb-grid") res = rando::LebRoom::parse(data, kind == "leb-grid").repack();
            else if (kind == "lvb") res = rando::LvbLevel::parse(data).repack();
            else if (kind == "gsheet") res = rando::GSheet::parse(data).write();
            else if (kind == "msbt") res = rando::Msbt::parse(data).pack();
            else if (kind == "sarc") res = rando::Sarc::parse(data).write();
            else if (kind == "bfevfl") res = rando::EventFlow::parse(data).write();
            else { fprintf(stderr, "unknown kind %s\n", kind.c_str()); return 2; }
        } catch (const std::exception &e) {
            res = std::string("ERROR ") + e.what();
            bad++;
        }
        std::string dst = out + "/" + (path[0] == '/' ? path.substr(1) : path);
        mkdirs(dst);
        std::ofstream(dst, std::ios::binary) << res;
        n++;
    }
    fprintf(stderr, "%d files, %d errors\n", n, bad);
    return 0;
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

// lasrando check-steps: every ported step against the record (tools/rando/test_apply.py drives it)
static int check_steps(const std::string &romfs, const std::string &decisions, const std::string &rec) {
    std::string dj, lj;
    if (!slurp(decisions, dj) || !slurp(rec + "/write_log.json", lj)) { fprintf(stderr, "can't read the inputs\n"); return 2; }
    rando::Decisions d;
    if (!d.load(dj)) { fprintf(stderr, "bad decisions\n"); return 2; }
    yyjson_doc *log = yyjson_read(lj.data(), lj.size(), 0);
    yyjson_val *L = yyjson_doc_get_root(log);
    size_t n = yyjson_arr_size(L);
    auto field = [&](size_t i, const char *k) { return yyjson_arr_get(L, i) ? yyjson_obj_get(yyjson_arr_get(L, i), k) : nullptr; };
    auto step_of = [&](size_t i) { const char *s = yyjson_get_str(field(i, "step")); return std::string(s ? s : ""); };
    auto path_of = [&](size_t i) {  // (romfs/<path>: the build's paths are within the romfs)
        std::string p = yyjson_get_str(field(i, "path"));
        return p.compare(0, 6, "romfs/") ? p : p.substr(6);
    };
    int bad = 0;
    for (const rando::BuildStep &st : rando::build_steps()) {
        // (a step named "a|b|c" is one generator call whose writes the log names by the helper that wrote them: its
        // writes start at the first one named a and go on while named a, b or c; "p>a|b|c": writes named p just
        // before that one belong to it too)
        std::string nm = st.name, pre;
        if (nm.find('>') != std::string::npos) pre = "|" + nm.substr(0, nm.find('>')) + "|", nm = nm.substr(nm.find('>') + 1);
        std::string anchor = nm.substr(0, nm.find('|')), names = "|" + nm + "|";
        auto in = [&](const std::string &set, size_t i) { return set.find("|" + step_of(i) + "|") != std::string::npos; };
        size_t k = 0;
        while (k < n && step_of(k) != anchor) k++;
        if (k == n) { printf("%-50s not in this seed's run\n", st.name); continue; }
        while (k > 0 && in(pre, k - 1)) k--;
        size_t end = k;
        while (end < n && (in(names, end) || in(pre, end))) end++;
        rando::SeedBuild b([&](const std::string &p, std::string &out) { return slurp(romfs + "/" + p, out); }, d);
        b.set_game_list([&](const std::string &dir, std::vector<std::string> &names) { return list_dir(romfs + "/" + dir, names); });
        for (size_t i = 0; i < k; i++) {  // the Python's files just before the step
            std::string bytes;
            slurp(rec + "/" + std::to_string(i) + ".bin", bytes);
            b.preload(path_of(i), bytes);
        }
        b.set_idgen(k ? (uint32_t)yyjson_get_uint(field(k - 1, "idgen")) : 0);
        std::string err;
        try {
            st.run(b);
        } catch (const std::exception &e) {
            err = e.what();
        }
        const auto &w = b.writes();
        size_t same = 0;
        std::string first;
        for (size_t i = 0; i < w.size() && k + i < end; i++) {
            std::string want;
            slurp(rec + "/" + std::to_string(k + i) + ".bin", want);
            if (w[i].path == path_of(k + i) && w[i].bytes == want) { same++; continue; }
            if (first.empty())
                first = "write " + std::to_string(i) + ": " + w[i].path + (w[i].path != path_of(k + i) ? " (the Python wrote " + path_of(k + i) + ")" : " differs");
        }
        bool ok = err.empty() && same == w.size() && w.size() == end - k;
        printf("%-50s %s: %zu of %zu writes the same%s%s\n", st.name, ok ? "ok" : "DIFFERENT", same, end - k,
               first.empty() ? "" : ("; first: " + first).c_str(), err.empty() ? "" : ("; error: " + err).c_str());
        bad += !ok;
    }
    yyjson_doc_free(log);
    return bad ? 1 : 0;
}

// lasrando dump: the full build, written out (tools/rando/test_apply.py compares it with a seed folder)
static int dump(const std::string &romfs, const std::string &decisions, const std::string &out) {
    std::string dj;
    if (!slurp(decisions, dj)) { fprintf(stderr, "can't read %s\n", decisions.c_str()); return 2; }
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
    if (!slurp(decisions, dj)) { fprintf(stderr, "can't read %s\n", decisions.c_str()); return 2; }
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

int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "dump")) return dump(argv[2], argv[3], argv[4]);
    if (argc >= 5 && !strcmp(argv[1], "serve-check")) return serve_check(argv[2], argv[3], argv[4]);
    if (argc >= 5 && !strcmp(argv[1], "generate"))
        return rando::solve::generate_main(argv[2], argv[3], argv[4], argc > 5 && !strcmp(argv[5], "--legacy"));
    if (argc >= 4 && !strcmp(argv[1], "entrances-check")) return rando::solve::entrances_check(argv[2], argv[3]);
    if (argc >= 5 && !strcmp(argv[1], "check-steps")) return check_steps(argv[2], argv[3], argv[4]);
    if (argc >= 4 && !strcmp(argv[1], "repack")) return repack(argv[2], argv[3]);
    if (argc >= 5 && !strcmp(argv[1], "texture-check")) return rando::texture_check(argv[2], argv[3], argv[4]);
    if (argc >= 5 && !strcmp(argv[1], "decide-check")) return rando::decide_check(argv[2], argv[3], argv[4], argc > 5 ? argv[5] : "");
    if (argc >= 4 && !strcmp(argv[1], "solve-check")) return rando::solve::solve_check(argv[2], argv[3]);
    if (argc >= 5 && !strcmp(argv[1], "outputs-check")) return rando::solve::outputs_check(argv[2], argv[3], argv[4]);
    if (argc >= 5 && !strcmp(argv[1], "prepare-check")) return rando::solve::prepare_check(argv[2], argv[3], argv[4], argc > 5 ? argv[5] : "");
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
