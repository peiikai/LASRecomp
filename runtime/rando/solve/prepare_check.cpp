// lasrando prepare-check: the preparation against a recorded Python run (prepare.h).
#include <stdio.h>

#include <chrono>

#include "prepare.h"
#include "rooms.h"
#include <stdlib.h>

namespace rando {
namespace solve {

static std::string brief(const Value &v) {
    std::string s = v.repr();
    return s.size() > 160 ? s.substr(0, 160) + "..." : s;
}

// a == b with every dict in the same key order and the same types (1 and True differ here: JSON keeps them apart);
// the first difference in *where
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
    if (a.type() == b.type() && a == b) return true;
    *where = path + ": " + brief(a) + ", recorded " + brief(b);
    return false;
}

static int compare(const char *what, const Prepared &st, const Value &rec) {
    int bad = 0;
    std::string where;
    for (const char *k : {"settings", "item_defs", "logic_defs"}) {
        const Value &mine = std::string(k) == "settings" ? st.settings : std::string(k) == "item_defs" ? st.item_defs
                                                                                                     : st.logic_defs;
        if (!same(mine, rec.at(k), k, &where)) bad++, printf("DIFF %s %s\n", what, where.c_str());
    }
    if (!bad)
        printf("%s: same (%zu settings, %zu items, %zu logic entries, key order included)\n", what,
               st.settings.dict().size(), st.item_defs.dict().size(), st.logic_defs.dict().size());
    return bad;
}

int prepare_check(const std::string &romfs, const std::string &request, const std::string &record,
                  const std::string &room_plan_file) {
    Value req, pre;
    try {
        req = Value::load_json(request);
        pre = Value::load_json(record + "/pre_entrances.json");
    } catch (const PyError &e) {
        fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    GameFiles game = game_files_dir(romfs);
    Prepared st;
    auto t0 = std::chrono::steady_clock::now();
    try {
        st = prepare(req, game);
    } catch (const std::exception &e) {
        printf("prepare failed: %s\n", e.what());
        return 1;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (const std::string &s : st.said) printf("  (%s)\n", s.c_str());
    int bad = compare("pre_entrances", st, pre);
    printf("prepare: %.3f s\n", secs);

    if (st.settings.get("dungeon-room-shuffle") != Value("off")) {
        // the room section (rooms.h) from post_entrances.json, against prefill.json and the run's room plan (the
        // placements.json LARECOMP_CHECK_ONLY writes, or the seed folder's decisions.json, when given)
        Value post, prefill;
        try {
            post = Value::load_json(record + "/post_entrances.json");
            prefill = Value::load_json(record + "/prefill.json");
        } catch (const PyError &e) {
            fprintf(stderr, "%s\n", e.what());
            return 2;
        }
        st.settings = post.at("settings"), st.item_defs = post.at("item_defs"), st.logic_defs = post.at("logic_defs");
        int first = getenv("LARECOMP_ROOM_ATTEMPT") ? atoi(getenv("LARECOMP_ROOM_ATTEMPT")) : 0;
        RoomPlan plan;
        auto t1 = std::chrono::steady_clock::now();
        try {
            int at = plan_rooms(st, first, plan, [](const std::string &s) { printf("  (%s)\n", s.c_str()); });
            printf("room layout: attempt %d\n", at);
            st.said.clear();
            std::vector<std::string> shuffled;
            for (const auto &e : plan.layouts.dict()) shuffled.push_back(e.key);
            finish_prefill(st, shuffled);
        } catch (const std::exception &e) {
            printf("room section failed: %s\n", e.what());
            return 1;
        }
        printf("room section: %.3f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
        for (const std::string &s : st.said) printf("  (%s)\n", s.c_str());
        bad += compare("prefill", st, prefill);
        if (!room_plan_file.empty()) {
            Value rec;
            try {
                rec = Value::load_json(room_plan_file);
            } catch (const PyError &e) {
                fprintf(stderr, "%s\n", e.what());
                return 2;
            }
            Value rp = rec.get("room-plan");
            std::string where;
            if (!rp.is_dict()) bad++, printf("DIFF room-plan: none recorded in %s\n", room_plan_file.c_str());
            else {
                Value mine = Value::new_dict();
                mine.set("layouts", plan.layouts), mine.set("jumps", plan.jumps);
                // (the record's JSON: sort_keys in decisions.json - compare the layouts by content, the jumps in order)
                bool ok = same(mine.at("jumps"), rp.at("jumps"), "room-plan['jumps']", &where);
                if (ok) {
                    Value theirs = rp.at("layouts");
                    if (theirs.is_list()) {  // (decisions.json: [[level, [[cell, room]...]]...], in order)
                        Value d = Value::new_dict();
                        for (const Value &lv : theirs.list()) {
                            Value cells = Value::new_dict();
                            for (const Value &c : lv.list()[1].list()) cells.set(c.list()[0].s(), c.list()[1]);
                            d.set(lv.list()[0].s(), cells);
                        }
                        theirs = d;
                        ok = same(mine.at("layouts"), theirs, "room-plan['layouts']", &where);
                    }
                    ok = ok && mine.at("layouts") == theirs;
                    if (!ok) where = "room-plan['layouts']: differ";
                }
                if (!ok) bad++, printf("DIFF %s\n", where.c_str());
                else printf("room-plan: same (%zu dungeons, %zu jump rows)\n", plan.layouts.dict().size(), plan.jumps.len());
            }
        }
    } else {
        Value post, prefill;
        try {
            post = Value::load_json(record + "/post_entrances.json");
            prefill = Value::load_json(record + "/prefill.json");
        } catch (const PyError &e) {
            fprintf(stderr, "%s\n", e.what());
            return 2;
        }
        st.said.clear();
        st.settings = post.at("settings"), st.item_defs = post.at("item_defs"), st.logic_defs = post.at("logic_defs");
        try {
            finish_prefill(st);
        } catch (const std::exception &e) {
            printf("finish_prefill failed: %s\n", e.what());
            return 1;
        }
        for (const std::string &s : st.said) printf("  (%s)\n", s.c_str());
        bad += compare("prefill", st, prefill);
    }
    printf("%s\n", bad ? "MISMATCH" : "EXACT");
    return bad ? 1 : 0;
}

}  // namespace solve
}  // namespace rando
