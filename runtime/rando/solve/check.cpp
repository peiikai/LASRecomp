// lasrando solve-check: the native fill against a recorded Python run (check.h).
#include "check.h"

#include <stdio.h>

#include <chrono>

#include "shuffler.h"

namespace rando {
namespace solve {

static std::string brief(const Value &v) {
    std::string s = v.repr();
    return s.size() > 160 ? s.substr(0, 160) + "..." : s;
}

// a == b, and every dict in the same key order; the first difference in *where
static bool same(const Value &a, const Value &b, const std::string &path, std::string *where) {
    if (a.is_dict() && b.is_dict()) {
        std::vector<std::string> ka = a.dict().keys(), kb = b.dict().keys();
        for (size_t i = 0; i < ka.size() || i < kb.size(); i++) {
            if (i >= ka.size() || i >= kb.size() || ka[i] != kb[i]) {
                *where = path + ": key " + std::to_string(i) + " is " + (i < ka.size() ? py_repr(ka[i]) : "(none)") +
                         ", recorded " + (i < kb.size() ? py_repr(kb[i]) : "(none)");
                return false;
            }
            if (!same(a.at(ka[i]), b.at(kb[i]), path + "[" + py_repr(ka[i]) + "]", where)) return false;
        }
        return true;
    }
    if (a.is_list() && b.is_list()) {
        const List &x = a.list(), &y = b.list();
        for (size_t i = 0; i < x.size() || i < y.size(); i++) {
            if (i >= x.size() || i >= y.size()) {
                *where = path + ": " + std::to_string(x.size()) + " items, recorded " + std::to_string(y.size());
                return false;
            }
            if (!same(x[i], y[i], path + "[" + std::to_string(i) + "]", where)) return false;
        }
        return true;
    }
    if (a == b) return true;
    *where = path + ": " + brief(a) + ", recorded " + brief(b);
    return false;
}

int solve_check(const std::string &prefill_path, const std::string &postfill_path) {
    Value pre, post;
    try {
        pre = Value::load_json(prefill_path);
        post = Value::load_json(postfill_path);
    } catch (const PyError &e) {
        fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    Value settings = pre.at("settings"), item_defs = pre.at("item_defs"), logic_defs = pre.at("logic_defs");
    ItemShuffler sh(settings.at("seed").s(), settings, item_defs, logic_defs.deepcopy());
    sh.compare_libm_pow(true);
    std::vector<std::string> said;
    sh.log = [&](const std::string &s) { said.push_back(s); };
    Value placements;
    auto t0 = std::chrono::steady_clock::now();
    try {
        placements = sh.run();
    } catch (const PyError &e) {
        printf("fill failed: %s\n", e.what());
        return 1;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (const std::string &s : said) printf("  (fill said: %s)\n", s.c_str());

    int bad = 0;
    std::string where;
    const Value &rec = post.at("placements");
    size_t locs = 0, placed = 0;
    for (const auto &e : placements.dict())
        if (sh.logic_defs.has(e.key) && sh.logic_defs.at(e.key).get("type") == Value("item")) locs++, placed += !e.value.is_none();
    if (!same(placements, rec, "placements", &where)) bad++, printf("DIFF %s\n", where.c_str());
    else
        printf("placements: same (%zu keys, %zu item locations, %zu indexes, key order included)\n", placements.dict().size(),
               locs, placements.at("indexes").dict().size());

    // random.getstate(): [3, [624 words + index], None]
    const List &rs = post.at("randstate").list();
    std::vector<int64_t> mine = sh.random_state();
    bool rs_ok = rs.size() == 3 && rs[0] == Value(sh.random_version()) && rs[2].is_none() && rs[1].list().size() == mine.size();
    for (size_t i = 0; rs_ok && i < mine.size(); i++) rs_ok = rs[1].list()[i] == Value((long long)mine[i]);
    if (!rs_ok) bad++, printf("DIFF random state (index %lld here)\n", (long long)mine.back());
    else printf("random state: same (all 625 words, version 3, gauss None)\n");

    if (!same(sh.settings, post.at("settings"), "settings", &where)) bad++, printf("DIFF %s\n", where.c_str());
    else printf("settings after the fill: same\n");

    printf("native fill: %.3f s, %ld searches; pacing pow: %ld calls, this platform's pow() differs on %ld, "
           "changes %ld paced orders\n",
           secs, sh.stats.searches, sh.stats.pow_calls, sh.stats.pow_libm_differs, sh.stats.pow_order_changes);
    printf("undone placements: %ld dungeon, %ld fill, %ld for shells, %ld for Triforce pieces; Tarin: %ld tried\n",
           sh.stats.undo_dungeon, sh.stats.undo_fill, sh.stats.undo_shells, sh.stats.undo_triforce, sh.stats.tarin_tries);
    printf("%s\n", bad ? "MISMATCH" : "EXACT");
    return bad ? 1 : 0;
}

}  // namespace solve
}  // namespace rando
