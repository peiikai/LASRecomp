// Decide mode (decide.h) for the fields the LAS steps read and the generator settles before ModsProcess writes a file:
// "las-globals" (the module globals las_headless.py sets), "songs" (mod_generator.randomizeMusic) and "chest-looks"
// (tools/rando/chest_looks.install). What they hold: tools/rando/record_las_items.py.
#include <math.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "../solve/prepare_lib.h"
#include "decide.h"
#include "las_data.h"
#include "las_lib.h"
#include "steps.h"

namespace rando {
namespace las {

namespace {

std::string jq(const std::string &s) {  // (a JSON string)
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

// las-globals: pets.enabled's ghost (item_get.GHOST_PET), triforce.counts' needed (chests.TRIFORCE,
// item_get.TRIFORCE_NEEDED), the souls' item keys (chests.SOUL_KEYS: souls.souls(souls.enabled(settings)))
std::string las_globals(const Decisions &d) {
    yyjson_val *s = d.raw("settings");
    bool ghost = py_truthy(yyjson_obj_get(s, "pet-ghost"));
    int64_t needed = 0;
    if (yyjson_val *t = yyjson_obj_get(s, "triforce-hunt"); py_truthy(t)) needed = (int64_t)yyjson_get_num(t);
    if (needed < 0) needed = 0;
    std::string keys;
    const solve::Value &souls = solve::prep_data().at("souls");
    auto add = [&](const char *prefix, const std::string &name) {
        std::string k = prefix;
        for (char c : name)
            if (c != ' ') k += c;
        keys += (keys.empty() ? "" : ",") + jq(k);
    };
    if (py_truthy(yyjson_obj_get(s, "boss-souls")))
        for (const solve::Value &b : souls.at("bosses").list()) add("BossSoul", b.list().at(1).s());
    if (py_truthy(yyjson_obj_get(s, "miniboss-souls")))
        for (const solve::Value &m : souls.at("minibosses").list()) add("MinibossSoul", m.list().at(0).s());
    return std::string("{\"ghost-pet\":") + (ghost ? "true" : "false") + ",\"soul-keys\":[" + keys +
           "],\"triforce\":" + (needed ? "true" : "false") + ",\"triforce-needed\":" + std::to_string(needed) + "}";
}

// randomizeMusic: the BGM list shuffled, then each track takes random.choice of what is left (bgms.index of the
// pick: the tracks are distinct, so its own place); the stream is then re-seeded with the seed
std::string songs(Decider &dc) {
    PyRandom &r = dc.shared();
    std::vector<const char *> bgms(data::BGM_TRACKS.begin(), data::BGM_TRACKS.end());
    r.shuffle(bgms);
    std::string o = "{";
    for (const char *t : data::BGM_TRACKS) {
        size_t i = r.choice_index(bgms.size());
        if (o.size() > 1) o += ",";
        o += jq(t) + ":" + jq(bgms[i]);
        bgms.erase(bgms.begin() + (long)i);
    }
    dc.reseed_shared();
    return o + "}";
}

// ModsProcess.run's start: the globals las_headless.py set before it, and the songs it draws first
void decide_start(SeedBuild &b) {
    Decider *dc = b.decider();
    if (!dc) return;
    dc->set("las-globals", las_globals(b.decisions()));
    if (py_truthy(setting_val(b.decisions(), "randomize-music"))) dc->set("songs", songs(*dc));
}

// ---- chest_looks: a logic condition -> true, false or the names it can need (_tokens)
struct Need {
    int b = -1;  // 1 / 0: always / never met; -1: the names
    std::set<std::string> names;
};

Need tokens(const std::string &cond, yyjson_val *settings) {
    std::vector<std::string> toks;  // re.findall(r'[()&|!]|[a-zA-Z0-9\-]+(?:\[[a-z]+\])?(?::\d+)?')
    auto word = [](char c) { return isalnum((unsigned char)c) || c == '-'; };
    for (size_t i = 0; i < cond.size();) {
        char c = cond[i];
        if (strchr("()&|!", c)) { toks.push_back(std::string(1, c)), i++; continue; }
        if (!word(c)) { i++; continue; }
        size_t j = i;
        while (j < cond.size() && word(cond[j])) j++;
        if (j < cond.size() && cond[j] == '[') {
            size_t k = j + 1;
            while (k < cond.size() && cond[k] >= 'a' && cond[k] <= 'z') k++;
            if (k > j + 1 && k < cond.size() && cond[k] == ']') j = k + 1;
        }
        if (j < cond.size() && cond[j] == ':' && j + 1 < cond.size() && isdigit((unsigned char)cond[j + 1])) {
            j++;
            while (j < cond.size() && isdigit((unsigned char)cond[j])) j++;
        }
        toks.push_back(cond.substr(i, j - i));
        i = j;
    }
    if (toks.empty()) return Need{1, {}};
    size_t pos = 0;
    auto join = [](const std::vector<Need> &vals, char op) {
        int win = op == '|' ? 1 : 0;
        for (const Need &v : vals)
            if (v.b == win) return Need{win, {}};
        Need out;
        bool any = false;
        for (const Need &v : vals)
            if (v.b < 0) any = true, out.names.insert(v.names.begin(), v.names.end());
        if (!any) return Need{op == '&' ? 1 : 0, {}};
        return out;
    };
    std::function<Need()> expr;
    std::function<Need()> atom = [&]() -> Need {
        const std::string t = toks.at(pos++);
        if (t == "(") {
            Need v = expr();
            pos++;  // ')'
            return v;
        }
        if (t == "!") {
            Need v = atom();
            return v.b >= 0 ? Need{!v.b, {}} : Need{-1, {}};
        }
        std::string name = t.substr(0, t.find(':'));
        yyjson_val *v = yyjson_obj_get(settings, name.substr(0, name.find('[')).c_str());
        if (yyjson_is_bool(v)) return Need{yyjson_get_bool(v) ? 1 : 0, {}};
        return Need{-1, {name}};
    };
    expr = [&]() -> Need {
        std::vector<Need> ors, ands{atom()};
        while (pos < toks.size() && toks[pos] != ")") {
            std::string op = toks[pos++];
            if (op == "&") {
                ands.push_back(atom());
            } else {
                ors.push_back(join(ands, '&'));
                ands = {atom()};
            }
        }
        ors.push_back(join(ands, '&'));
        return join(ors, '|');
    };
    return expr();
}

std::string str_of(yyjson_val *v) { const char *s = yyjson_get_str(v); return s ? s : ""; }

// chest_looks.install: each chest's look (vanilla / key / plain), a trap's from a random real item of the seed
// (random.Random(f"{seed}-chest-looks")), and Dampe's panel chests'
std::string chest_looks(SeedBuild &b, Decider &dc) {
    const Decisions &d = b.decisions();
    const std::string *lj = dc.input("logic.json");  // (the solver's logic after the fill: sh.logic_defs)
    if (!lj) throw std::runtime_error("decide: chest looks need the seed's logic (logic.json)");
    yyjson_doc *ldoc = yyjson_read(lj->data(), lj->size(), 0);
    if (!ldoc) throw std::runtime_error("decide: bad logic.json");
    yyjson_val *logic = yyjson_doc_get_root(ldoc), *settings = d.raw("settings"), *defs = d.raw("item-defs"), *k, *v;
    std::set<std::string> skip;
    for (const char *f : {"excluded-locations", "absent-locations", "junk-locations"}) {
        yyjson_arr_iter it = yyjson_arr_iter_with(yyjson_obj_get(settings, f));
        while ((v = yyjson_arr_iter_next(&it))) skip.insert(str_of(v));
    }
    // needed_items
    std::vector<std::string> todo;
    yyjson_obj_iter oi = yyjson_obj_iter_with(logic);
    while ((k = yyjson_obj_iter_next(&oi))) {
        std::string t = str_of(yyjson_obj_get(yyjson_obj_iter_get_val(k), "type"));
        if ((t == "item" || t == "follower") && !skip.count(yyjson_get_str(k))) todo.push_back(yyjson_get_str(k));
    }
    todo.push_back("kill-nightmare");
    std::set<std::string> seen, needed;
    while (!todo.empty()) {
        std::string name = todo.back();
        todo.pop_back();
        size_t br = name.find('[');
        std::string base = name.substr(0, br), param = br == std::string::npos ? "" : name.substr(br + 1);
        yyjson_val *node = yyjson_obj_get(logic, base.c_str());
        if (seen.count(name) || !node) {
            if (yyjson_obj_get(defs, base.c_str())) needed.insert(base);
            continue;
        }
        seen.insert(name);
        std::vector<yyjson_val *> conds{yyjson_obj_get(node, "condition-basic")};
        if (!param.empty()) conds.push_back(yyjson_obj_get(node, ("condition-" + param.substr(0, param.size() - 1)).c_str()));
        if (py_truthy(yyjson_obj_get(node, "region"))) todo.push_back(str_of(yyjson_obj_get(node, "region")));
        for (yyjson_val *c : conds) {
            if (!yyjson_is_str(c)) continue;
            Need n = tokens(yyjson_get_str(c), settings);
            if (n.b < 0) todo.insert(todo.end(), n.names.begin(), n.names.end());  // (a set: any order)
        }
    }
    yyjson_doc_free(ldoc);
    auto is_trap = [](const std::string &i) { return i.size() >= 5 && !i.compare(i.size() - 5, 5, "-trap"); };
    auto look_of = [&](const std::string &item) -> std::string {
        if (!item.compare(0, 4, "key-") || !item.compare(0, 14, "nightmare-key-")) return "key";
        std::string t = str_of(yyjson_obj_get(yyjson_obj_get(defs, item.c_str()), "type"));
        if (t == "important" || t == "triforce" || needed.count(item)) return "vanilla";
        return "plain";
    };
    auto file_of = [](const std::string &look) {
        return std::string(look == "key" ? "ObjTreasureBoxKey.bfres" : look == "plain" ? "ObjTreasureBoxPlain.bfres" : "ObjTreasureBox.bfres");
    };
    std::vector<std::string> real;
    yyjson_obj_iter pi = yyjson_obj_iter_with(d.raw("placements"));
    while ((k = yyjson_obj_iter_next(&pi))) {
        std::string item = str_of(yyjson_obj_iter_get_val(k));
        if (strcmp(yyjson_get_str(k), "settings") && !is_trap(item) && yyjson_obj_get(defs, item.c_str())) real.push_back(item);
    }
    std::sort(real.begin(), real.end());
    PyRandom rng = dc.stream("chest-looks");
    std::vector<std::string> rooms;
    for (const auto &r : data::CHEST_ROOMS) rooms.push_back(r.first);
    std::sort(rooms.begin(), rooms.end());
    std::string o = "{\"panel\":";
    std::string looks;
    for (const std::string &loc : rooms) {
        const std::string *item = d.item_at(loc);
        if (!item) continue;
        std::string it = *item;
        if (is_trap(it)) it = rng.choice(real);
        looks += (looks.empty() ? "" : ",") + jq(loc) + ":" + jq(file_of(look_of(it)));
    }
    bool stand_out = false;  // (Dampe's rewards: randomizer_data.DAMPE_REWARDS)
    for (const solve::Value &loc : solve::prep_data().at("groups").at("DAMPE_REWARDS").list()) {
        const std::string *i = d.item_at(loc.s());
        if (i && !i->empty() && !is_trap(*i) && look_of(*i) != "plain") stand_out = true;
    }
    return o + jq(file_of(stand_out ? "vanilla" : "plain")) + ",\"rooms\":{" + looks + "}}";
}

}  // namespace

void decide_chest_looks(SeedBuild &b) {
    Decider *dc = b.decider();
    if (!dc || !py_truthy(setting_val(b.decisions(), "chest-looks")) || b.decisions().raw("chest-looks")) return;
    dc->set("chest-looks", chest_looks(b, *dc));
}

std::vector<BuildStep> steps_las_decide() { return {{"mod_generator.randomizeMusic", decide_start}}; }

}  // namespace las
}  // namespace rando
