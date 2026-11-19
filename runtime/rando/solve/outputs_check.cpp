// lasrando outputs-check: the seed's text outputs rebuilt natively from a recorded run and compared with the seed
// folder the Python wrote (outputs.h).
#include <stdio.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

#include "outputs.h"
#include "shuffler.h"

namespace rando {
namespace solve {

static bool read_all(const std::string &path, std::string &data) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static std::string base_name(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// where two texts first differ: "line N: <ours> | <theirs>"
static std::string first_diff(const std::string &a, const std::string &b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
    size_t line = 1;
    for (size_t k = 0; k < i; k++)
        line += a[k] == '\n';
    auto cut = [&](const std::string &s) {
        size_t from = i > 60 ? i - 60 : 0;
        std::string t = s.substr(std::min(from, s.size()), 140);
        for (char &ch : t)
            if (ch == '\n') ch = '/';
        return t;
    };
    return "byte " + std::to_string(i) + " (line " + std::to_string(line) + "), sizes " + std::to_string(a.size()) + " / " +
           std::to_string(b.size()) + "\n      ours:   " + cut(a) + "\n      python: " + cut(b);
}

int outputs_check(const std::string &romfs, const std::string &record, const std::string &seed_dir) {
    Value pre, post, ent, dec;
    try {
        pre = Value::load_json(record + "/prefill.json");
        post = Value::load_json(record + "/postfill.json");
        std::string tmp;
        if (read_all(record + "/post_entrances.json", tmp)) ent = Value::parse_json(tmp.data(), tmp.size()).get("ent");
        if (read_all(seed_dir + "/decisions.json", tmp)) dec = Value::parse_json(tmp.data(), tmp.size());
    } catch (const PyError &e) {
        fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    auto t0 = std::chrono::steady_clock::now();
    Value settings = pre.at("settings"), item_defs = pre.at("item_defs"), logic_defs = pre.at("logic_defs");
    ItemShuffler sh(settings.at("seed").s(), settings, item_defs, logic_defs.deepcopy());
    Value placements;
    try {
        placements = sh.run();
    } catch (const PyError &e) {
        printf("fill failed: %s\n", e.what());
        return 1;
    }
    double fill_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int bad = 0;
    if (placements != post.at("placements") || placements.dict().keys() != post.at("placements").dict().keys())
        bad++, printf("DIFF the fill's placements aren't the recorded ones (lasrando solve-check tells where)\n");
    OutputsInput in;
    in.settings = sh.settings;
    in.placements = placements;
    in.logic_defs = sh.logic_defs;
    in.ent = ent;
    in.final_settings = dec.truthy() ? dec.get("settings") : Value();
    in.romfs = romfs;
    in.folder_name = base_name(seed_dir);
    in.read_seed = [&](const std::string &rel, std::string &data) { return read_all(seed_dir + "/" + rel, data); };
    Outputs res;
    auto t1 = std::chrono::steady_clock::now();
    try {
        res = build_outputs(in);
    } catch (const PyError &e) {
        printf("outputs failed: %s\n", e.what());
        return 1;
    }
    double out_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    for (const std::string &l : res.log) printf("  (%s)\n", l.c_str());
    if (res.exit_code) printf("unbeatable: exit %d\n", res.exit_code);
    std::vector<std::string> made;
    for (const auto &f : res.files) {
        made.push_back(f.first);
        std::string theirs;
        if (!read_all(seed_dir + "/" + f.first, theirs)) {
            bad++, printf("DIFF %-18s the seed folder has none\n", f.first.c_str());
            continue;
        }
        if (theirs == f.second) printf("same %-18s %zu bytes\n", f.first.c_str(), f.second.size());
        else bad++, printf("DIFF %-18s %s\n", f.first.c_str(), first_diff(f.second, theirs).c_str());
    }
    static const char *const kAll[] = {"logic.json", "spheres.json", "hint_steps.json", "route.json", "seed_hints.json",
                                       "spoiler.txt", "settings.json", "tracker.json", "tracker.tsv", "spoiler_view.txt",
                                       "spoiler.html"};
    for (const char *n : kAll) {
        struct stat st;
        if (std::find(made.begin(), made.end(), n) == made.end() && stat((seed_dir + "/" + n).c_str(), &st) == 0)
            bad++, printf("DIFF %-18s not made here\n", n);
    }
    printf("fill %.2f s, outputs %.2f s\n%s\n", fill_s, out_s, bad ? "MISMATCH" : "EXACT");
    return bad ? 1 : 0;
}

}  // namespace solve
}  // namespace rando
