// lasrando decide-check (decide.h): a seed built in decide mode from the item fill's result, against the seed the
// Python generator made: its decisions document, its tables and its files.
#include "decide_check.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fstream>
#include <iterator>
#include <set>

#include "../mkdirs.h"
#include "decide.h"
#include "steps.h"
#include "../../../third_party/yyjson/yyjson.h"

namespace rando {

// the fields the build decides (tools/rando/record*.py); the others are the fill's and the solver's
const char *const DECIDED_FIELDS[] = {"trap-models", "songs", "chest-looks", "las-globals", "enemies", "chest-enemy",
                                      "shop-prices", "hint-sheet", "seed-hints", "ours-world"};
// the seed folder's tables (seed.json "tables"; docs/randomizer.md)
const char *const SEED_TABLES[] = {"pots.tsv", "bushes.tsv", "rocks.tsv", "digs.tsv", "shell_spots.tsv",
                                   "apple_trees.tsv", "shopsanity.tsv", "souls.tsv", "triforce.tsv",
                                   "quick_pickups.tsv", "hints.tsv", "flags.tsv", "flags_ext", "roam.tsv",
                                   "enemies_random.tsv", "check_flags.tsv"};
// written by steps not ported yet (tools/rando/test_apply.py NOT_PORTED)
static const char *const NOT_PORTED[] = {"romfs/region_common/ui/StartUp.arc",
                                         "romfs/region_common/actor/ObjTreasureBoxKey.bfres",
                                         "romfs/region_common/actor/ObjTreasureBoxPlain.bfres"};

static bool slurp(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

static bool list_dir(const std::string &path, std::vector<std::string> &names) {
    DIR *d = opendir(path.c_str());
    if (!d) return false;
    while (struct dirent *e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    return true;
}

static void walk(const std::string &top, const std::string &rel, std::set<std::string> &out) {
    std::vector<std::string> names;
    if (!list_dir(top + "/" + rel, names)) return;
    for (const std::string &n : names) {
        std::string r = rel + "/" + n;
        struct stat st;
        if (stat((top + "/" + r).c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(top, r, out);
        else out.insert(r);
    }
}

static void mkdirs(const std::string &path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') make_dir(path.substr(0, i));
}

// where two texts first differ, with some of each around it
static std::string first_diff(const std::string &a, const std::string &b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
    size_t from = i > 60 ? i - 60 : 0;
    auto cut = [&](const std::string &s) {
        std::string t = s.substr(std::min(from, s.size()), 140);
        for (char &c : t)
            if (c == '\n') c = ' ';
        return t;
    };
    char buf[32];
    snprintf(buf, sizeof buf, "at byte %zu", i);
    return std::string(buf) + "\n        made: ..." + cut(a) + "\n        want: ..." + cut(b);
}

static std::string field_json(yyjson_val *v) {
    if (!v) return "(none)";
    size_t n;
    char *t = yyjson_val_write(v, 0, &n);
    std::string s = py_json(t, n, 1, true);
    free(t);
    return s;
}

int decide_check(const std::string &romfs, const std::string &rec, const std::string &seed, const std::string &out) {
    std::string dj, pj;
    if (!slurp(seed + "/decisions.json", dj) || !slurp(rec + "/postfill.json", pj)) {
        fprintf(stderr, "can't read %s/decisions.json or %s/postfill.json\n", seed.c_str(), rec.c_str());
        return 2;
    }
    Decisions d;
    if (!d.load(dj)) { fprintf(stderr, "bad decisions\n"); return 2; }
    for (const char *f : DECIDED_FIELDS) d.drop_field(f);
    std::vector<uint32_t> rs;
    {
        yyjson_doc *p = yyjson_read(pj.data(), pj.size(), 0);
        yyjson_val *words = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(p), "randstate"), 1), *w;
        yyjson_arr_iter it = yyjson_arr_iter_with(words);
        while ((w = yyjson_arr_iter_next(&it))) rs.push_back((uint32_t)yyjson_get_uint(w));
        yyjson_doc_free(p);
    }
    Decider dc(d, rs);
    for (const char *in : {"hint_steps.json", "seed_hints.json", "logic.json"}) {
        std::string bytes;
        if (slurp(seed + "/" + in, bytes)) dc.set_input(in, bytes);
    }
    {
        std::string bytes;
        if (slurp(rec + "/post_entrances.json", bytes)) dc.set_input("post_entrances.json", bytes);
    }
    SeedBuild b([&](const std::string &p, std::string &o) { return slurp(romfs + "/" + p, o); }, d);
    b.set_game_list([&](const std::string &dir, std::vector<std::string> &names) { return list_dir(romfs + "/" + dir, names); });
    b.set_decider(&dc);
    std::string err;
    for (const BuildStep &st : build_steps()) {
        if (!build_step_runs(st.name, d)) continue;
        try {
            st.run(b);
        } catch (const std::exception &e) {
            err = std::string(st.name) + ": " + e.what();
            break;
        }
    }
    if (!err.empty()) printf("build stopped: %s\n", err.c_str());
    int bad = !err.empty();

    // the decisions, field by field
    std::string doc;
    try {
        doc = dc.document();
    } catch (const std::exception &e) {
        printf("decisions: can't write them: %s\n", e.what());
        return 1;
    }
    if (doc == dj) {
        printf("decisions: the same, every field (%zu bytes)\n", dj.size());
    } else {
        bad++;
        yyjson_doc *want = yyjson_read(dj.data(), dj.size(), 0), *made = yyjson_read(doc.data(), doc.size(), 0);
        std::set<std::string> names;
        for (yyjson_doc *x : {want, made}) {
            yyjson_val *k;
            yyjson_obj_iter it = yyjson_obj_iter_with(yyjson_doc_get_root(x));
            while ((k = yyjson_obj_iter_next(&it))) names.insert(yyjson_get_str(k));
        }
        int same = 0;
        for (const std::string &n : names) {
            std::string a = field_json(yyjson_obj_get(yyjson_doc_get_root(made), n.c_str()));
            std::string w = field_json(yyjson_obj_get(yyjson_doc_get_root(want), n.c_str()));
            if (a == w) { same++; continue; }
            printf("decisions: %s DIFFERENT %s\n", n.c_str(), first_diff(a, w).c_str());
        }
        printf("decisions: %d of %zu fields the same\n", same, names.size());
        yyjson_doc_free(want), yyjson_doc_free(made);
    }

    // the tables
    int tsame = 0, tbad = 0;
    for (const char *t : SEED_TABLES) {
        std::string want;
        bool has = slurp(seed + "/" + t, want);
        const std::string *made = b.table(t);
        if (!has && !made) continue;
        if (has && made && *made == want) { tsame++; continue; }
        tbad++;
        if (!made) printf("table %s: MISSING (not made)\n", t);
        else if (!has) printf("table %s: EXTRA (the seed has none)\n", t);
        else printf("table %s: DIFFERENT %s\n", t, first_diff(*made, want).c_str());
    }
    printf("tables: %d the same, %d not\n", tsame, tbad);
    bad += tbad;

    // the files (as tools/rando/test_apply.py)
    std::set<std::string> want_files;
    walk(seed, "romfs", want_files), walk(seed, "exefs", want_files);
    int fsame = 0, fdiff = 0, fmiss = 0, fextra = 0, later = 0;
    std::string firstf;
    std::set<std::string> made_files;
    for (const auto &f : b.files()) made_files.insert(f.first.compare(0, 6, "exefs/") ? "romfs/" + f.first : f.first);
    for (const std::string &rel : want_files) {
        const std::string &r = rel;
        bool np = false;
        for (const char *n : NOT_PORTED) np |= r == n;
        if (np) { later++; continue; }
        std::string key = r.compare(0, 6, "romfs/") ? r : r.substr(6), want;
        auto it = b.files().find(key);
        if (it == b.files().end()) { fmiss++; if (firstf.empty()) firstf = "missing " + r; continue; }
        slurp(seed + "/" + r, want);
        if (it->second == want) fsame++;
        else { fdiff++; if (firstf.empty()) firstf = "differs " + r; }
    }
    for (const std::string &m : made_files)
        if (!want_files.count(m)) { fextra++; if (firstf.empty()) firstf = "extra " + m; }
    printf("files: %d the same, %d different, %d missing, %d extra (%d from steps not ported yet)%s%s\n", fsame, fdiff,
           fmiss, fextra, later, firstf.empty() ? "" : "; first: ", firstf.c_str());
    bad += fdiff + fmiss + fextra;

    if (!out.empty()) {  // (the build: decisions.json, the tables, romfs/ and exefs/)
        mkdirs(out + "/x");
        std::ofstream(out + "/decisions.json", std::ios::binary) << doc;
        for (const auto &t : b.tables()) std::ofstream(out + "/" + t.first, std::ios::binary) << t.second;
        for (const auto &f : b.files()) {
            std::string dst = out + "/" + (f.first.compare(0, 6, "exefs/") ? "romfs/" + f.first : f.first);
            mkdirs(dst);
            std::ofstream(dst, std::ios::binary) << f.second;
        }
    }
    return bad ? 1 : 0;
}

}  // namespace rando
