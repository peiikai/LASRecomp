// lasrando: the randomizer's command-line tool, built from the same code the game runs (runtime/rando/).
//
//   lasrando convert <seed folder> [<out.json>]   the folder's record as seed.json (an older folder: read from its
//                                                  settings.json + seed_id.txt), to <out.json> or stdout
//   lasrando get <seed folder> <key>...            settings as the runtime reads them: one line per key,
//                                                  "<key> bool=<0|1> int=<n|-> str=<s|->"
//   lasrando repack <kind> <out folder> < paths     each listed game file read and written back by the format code
//                                                  (kind: leb, leb-grid, lvb, gsheet, msbt, sarc, bfevfl), to <out folder>/<the path as listed>;
//                                                  tools/rando/test_formats.py compares that with the Python's
//   lasrando check-steps <romfs> <decisions.json> <record folder>
//                                                  each ported generator step (runtime/rando/apply/) run from the
//                                                  Python's state just before it (tools/rando/record.py's write log)
//                                                  and its writes compared with the Python's, write by write
//
// Build: make lasrando (build/lasrando).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "../runtime/rando/fmt/bfevfl.h"
#include "../runtime/rando/fmt/gsheet.h"
#include "../runtime/rando/fmt/leb.h"
#include "../runtime/rando/fmt/msbt.h"
#include "../runtime/rando/fmt/sarc.h"
#include "../runtime/rando/fmt/lvb.h"
#include "../runtime/rando/apply/seed_build.h"
#include "../runtime/rando/seed.h"
#include "../third_party/yyjson/yyjson.h"

static int usage(void) {
    fprintf(stderr, "usage: lasrando convert <seed folder> [<out.json>]\n"
                    "       lasrando get <seed folder> <key>...\n"
                    "       lasrando repack <leb|leb-grid|lvb|gsheet|msbt|bfevfl|sarc> <out folder> < paths\n");
    return 2;
}

static void mkdirs(const std::string &path) {  // (the folders of a file's path)
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
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
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
        size_t k = 0;
        while (k < n && step_of(k) != st.name) k++;
        if (k == n) { printf("%-50s not in this seed's run\n", st.name); continue; }
        size_t end = k;
        while (end < n && step_of(end) == st.name) end++;
        rando::SeedBuild b([&](const std::string &p, std::string &out) { return slurp(romfs + "/" + p, out); }, d);
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

int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "check-steps")) return check_steps(argv[2], argv[3], argv[4]);
    if (argc >= 4 && !strcmp(argv[1], "repack")) return repack(argv[2], argv[3]);
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
