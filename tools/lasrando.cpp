// lasrando: the randomizer's command-line tool, built from the same code the game runs (runtime/rando/).
//
//   lasrando convert <seed folder> [<out.json>]   the folder's record as seed.json (an older folder: read from its
//                                                  settings.json + seed_id.txt), to <out.json> or stdout
//   lasrando get <seed folder> <key>...            settings as the runtime reads them: one line per key,
//                                                  "<key> bool=<0|1> int=<n|-> str=<s|->"
//   lasrando repack <kind> <out folder> < paths     each listed game file read and written back by the format code
//                                                  (kind: leb, leb-grid, lvb, gsheet, msbt, sarc, bfevfl), to <out folder>/<the path as listed>;
//                                                  tools/rando/test_formats.py compares that with the Python's
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
#include "../runtime/rando/seed.h"

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

int main(int argc, char **argv) {
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
