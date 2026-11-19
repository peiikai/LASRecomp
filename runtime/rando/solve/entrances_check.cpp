// lasrando entrances-check: the native entrance shuffle against a recorded Python run (entrances.h).
#include <stdio.h>

#include <chrono>
#include <fstream>
#include <iterator>

#include "entrances.h"

namespace rando {
namespace solve {

namespace {

std::string brief(const Value &v) {
    std::string s = v.repr();
    return s.size() > 160 ? s.substr(0, 160) + "..." : s;
}

// a == b, and every dict in the same key order; the first difference in *where
bool same(const Value &a, const Value &b, const std::string &path, std::string *where) {
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
    if (a.type() == b.type() && a == b) return true;
    *where = path + ": " + brief(a) + ", recorded " + brief(b);
    return false;
}

}  // namespace

int entrances_check(const std::string &romfs, const std::string &record_dir) {
    Value pre, post;
    try {
        pre = Value::load_json(record_dir + "/pre_entrances.json");
        post = Value::load_json(record_dir + "/post_entrances.json");
    } catch (const PyError &e) {
        fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    GameRead read = [&](const std::string &path, std::string &bytes) {
        std::ifstream f(romfs + "/" + path, std::ios::binary);
        if (!f) return false;
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    };
    Value state = Value::new_dict();
    for (const char *k : {"settings", "item_defs", "logic_defs"}) state.set(k, pre.at(k));
    auto t0 = std::chrono::steady_clock::now();
    try {
        shuffle_entrances(state, read, [](const std::string &s) { printf("  (said: %s)\n", s.c_str()); });
    } catch (const PyError &e) {
        printf("entrance shuffle failed: %s\n", e.what());
        return 1;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int bad = 0;
    std::string where;
    for (const char *k : {"settings", "item_defs", "logic_defs", "ent"}) {
        if (!same(state.at(k), post.at(k), k, &where)) bad++, printf("DIFF %s\n", where.c_str());
        else if (state.at(k).is_dict()) printf("%s: same (%zu keys, key order included)\n", k, state.at(k).dict().size());
        else if (state.at(k).is_none()) printf("%s: same (None: no entrance shuffle)\n", k);
        else printf("%s: same (%zu doors, %zu entries, %zu exits)\n", k, state.at(k).list()[0].len(),
                    state.at(k).list()[1].len(), state.at(k).list()[2].len());
    }
    printf("native entrance shuffle: %.3f s\n%s\n", secs, bad ? "MISMATCH" : "EXACT");
    return bad ? 1 : 0;
}

}  // namespace solve
}  // namespace rando
