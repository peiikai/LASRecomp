// LAS Recomp's item steps: keysanity and bag texts, refills, silent pickups (tools/rando/keysanity_text.py,
// refill_gets.py, bomb_bag_text.py, powder_bag_text.py, silent_pickups.py), ported (ours_items.cpp).
//
// These run over every event flow the seed has written so far (the Python globs the seed's event folder: sorted
// file names), so they come after every other flow edit. Python's view of a parameter is kept: an evfl Argument is a
// str (it equals a string of the same text) unless the Python asks isinstance(..., Argument).
#include <string.h>

#include <map>
#include <set>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "ours_items_lib.h"

namespace rando {
namespace ours {
namespace {

using namespace las;

const std::string EV = "region_common/event/";

// the seed's event flows, sorted (glob(<seed>/romfs/region_common/event/*.bfevfl)): file names
std::vector<std::string> written_flows(SeedBuild &b) {
    std::vector<std::string> out;
    for (const auto &f : b.files()) {
        const std::string &p = f.first;
        if (p.compare(0, EV.size(), EV) || p.find('/', EV.size()) != std::string::npos) continue;
        if (p.size() < 7 || p.compare(p.size() - 7, 7, ".bfevfl")) continue;
        out.push_back(p.substr(EV.size()));
    }
    return out;
}

// an action event's actor, action and parameters (false: not an action event)
bool action_of(EvflFlowchart &fc, uint16_t i, std::string *actor, std::string *act, EvflContainer **prm) {
    if (i == EVFL_NONE || i >= fc.events.size()) return false;
    EvflEvent &e = fc.events[i];
    if (e.type != EvflEvent::Action) return false;
    if (actor) *actor = fc.actors.at(e.actor).id.name;
    if (act) *act = fc.actors.at(e.actor).actions.at(e.action);
    if (prm) *prm = &e.params;
    return true;
}
std::string act_name(EvflFlowchart &fc, uint16_t i) {
    std::string a;
    return action_of(fc, i, nullptr, &a, nullptr) ? a : "";
}

bool is_arg(const EvflValue *v) { return v && v->type == EvflValue::Argument; }
bool is_str(const EvflValue *v) { return v && (v->type == EvflValue::String || v->type == EvflValue::Argument); }
bool str_is(const EvflValue *v, const std::string &s) { return is_str(v) && v->s == s; }  // (v == s)
bool int_of(const EvflValue *v, int &out) {  // (an int, or a bool: Python's True == 1)
    if (!v) return false;
    if (v->type == EvflValue::Int) { out = v->i; return true; }
    if (v->type == EvflValue::Bool) { out = v->b; return true; }
    return false;
}
bool truthy(const EvflValue *v) {  // (bool(prm.get(k)))
    if (!v) return false;
    switch (v->type) {
    case EvflValue::Int: return v->i != 0;
    case EvflValue::Bool: return v->b;
    case EvflValue::Float: return v->f != 0;
    case EvflValue::String: case EvflValue::Argument: return !v->s.empty();
    default: return true;
    }
}
std::string name_of(EvflFlowchart &fc, uint16_t i) { return i == EVFL_NONE ? "" : fc.events.at(i).name; }

// Item.bfevfl copied from the game into the seed when it has none (the modules' plain file copy)
void ensure_item_flow(SeedBuild &b) {
    std::string ip = EV + "Item.bfevfl", bytes;
    if (b.files().count(ip)) return;
    if (!b.read_game(ip, bytes)) throw std::runtime_error("no " + ip);
    b.write_bytes(ip, bytes);
}

// an Item.bfevfl entry showing System:<entry>
void item_entry(Flow &f, const std::string &entry) {
    if (f.find_entry(entry)) return;
    f.add_entry_point(entry);
    f.action_chain(entry, {{"Dialog", "Show", {{"message", "System:" + entry}}}});
}

// ======== keysanity_text.py
struct KsItem { const char *prefix, *key, *msg; };
const KsItem KS_ITEMS[] = {{"key-", "SmallKey", "GetSmallKey"}, {"nightmare-key-", "NightmareKey", "GetBossKey"},
                           {"compass-", "Compass", "GetCompass"}, {"map-", "DungeonMap", "GetMap"},
                           {"stone-beak-", "StoneBeak", "GetStoneBeak"}};
using Combos = std::set<std::pair<std::string, int>>;  // (item key, dungeon index), sorted as the Python's sorted()

std::string ks_entry(const std::string &key, int idx) { return "Ks" + key + std::to_string(idx); }
std::string place_label(int idx) { return idx == 9 ? "ClothesDungeon_map" : "Lv" + std::to_string(idx + 1) + "Dungeon_map"; }

Combos ks_used(const Decisions &d) {
    Combos out;
    yyjson_val *idx = d.raw("indexes"), *k;
    yyjson_obj_iter it = yyjson_obj_iter_with(idx);
    while ((k = yyjson_obj_iter_next(&it))) {
        yyjson_val *v = yyjson_obj_iter_get_val(k);
        if (!yyjson_is_int(v)) continue;
        int i = (int)yyjson_get_sint(v);
        if (!((i >= 0 && i <= 7) || i == 9)) continue;
        std::string item = placed(d, yyjson_get_str(k));
        if (item.empty()) continue;
        // (the longest prefix: nightmare-key- before key-)
        const KsItem *best = nullptr;
        for (const KsItem &x : KS_ITEMS) {
            if (item.compare(0, strlen(x.prefix), x.prefix)) continue;
            if (!best || strlen(x.prefix) > strlen(best->prefix)) best = &x;
        }
        if (best) out.insert({best->key, i});
    }
    return out;
}

std::string ks_compose(const std::string &get_raw, const std::string &name_raw, bool english) {
    std::vector<Tok> t = tokens(get_raw);
    std::string name;
    for (const Tok &k : tokens(name_raw)) name += k.raw;
    std::string wait;
    bool has_wait = false;
    for (size_t i = t.size(); i-- > 0;)
        if (is_wait(t[i])) { wait = t[i].raw, has_wait = true; break; }
    std::vector<size_t> colors;
    for (size_t i = 0; i < t.size(); i++)
        if (t[i].is(COLOR[0], COLOR[1])) colors.push_back(i);
    if (english && has_wait && !wait.empty() && colors.size() >= 2) {
        std::string on = t[colors[0]].raw, off = t[colors[1]].raw, head;
        size_t shown = 0;
        for (size_t i = 0; i <= colors[1]; i++) {
            head += t[i].raw;
            if (!t[i].tag) shown += cp_len(t[i].raw);
        }
        std::string sep = shown + cp_len(name) + 6 <= 40 ? " for " : "\nfor ";
        return head + sep + on + name + off + "!" + wait + std::string(1, '\0');
    }
    if (!t.empty() && is_wait(t.back())) t.pop_back();
    std::string body, on, off;
    for (const Tok &k : t) body += k.raw;
    if (colors.size() >= 2) on = t.at(colors[0]).raw, off = t.at(colors[1]).raw;
    return body + wait + PAGE + on + name + off + wait + std::string(1, '\0');
}

bool is_action(EvflFlowchart &fc, uint16_t i, const char *actor, const char *act) {
    std::string a, b;
    return action_of(fc, i, &a, &b, nullptr) && a == actor && b == act;
}

int ks_patch_flow(SeedBuild &b, EventFlow &flow, const Combos &combos) {
    EvflFlowchart &fc = flow.flowchart;
    Flow f(b, fc);
    std::set<std::string> keys;
    for (const auto &c : combos) keys.insert(c.first);
    int n = 0;
    std::set<uint16_t> added, after_get;
    for (size_t i = 0; i < fc.events.size(); i++)
        if (is_action(fc, (uint16_t)i, "Link", "GenericItemGetSequenceByKey")) after_get.insert(fc.events[i].next);
    size_t count = fc.events.size();
    for (size_t i = 0; i < count; i++) {  // silent constant gives: a get event in front
        if (!is_action(fc, (uint16_t)i, "Inventory", "AddItemByKey") || after_get.count((uint16_t)i)) continue;
        const EvflValue *key = fc.events[i].params.get("itemKey"), *idx = fc.events[i].params.get("index");
        int ix;
        if (is_arg(key) || is_arg(idx) || !key || key->type != EvflValue::String || !int_of(idx, ix) ||
            !combos.count({key->s, ix}))
            continue;
        std::string k = key->s;
        std::string get = f.action("Link", "GenericItemGetSequenceByKey",
                                   {{"itemKey", k}, {"keepCarry", false}, {"messageEntry", ks_entry(k, ix)}}, fc.events[i].name);
        uint16_t g = event_index(fc, get);
        retarget(fc, (uint16_t)i, g, {g});
        added.insert(g);
        n++;
    }
    count = fc.events.size();
    for (size_t i = 0; i < count; i++) {
        if (!is_action(fc, (uint16_t)i, "Link", "GenericItemGetSequenceByKey") || added.count((uint16_t)i)) continue;
        uint16_t nx = fc.events[i].next;
        if (nx == EVFL_NONE || !is_action(fc, nx, "Inventory", "AddItemByKey")) continue;
        const EvflValue *key = fc.events[nx].params.get("itemKey"), *idx = fc.events[nx].params.get("index");
        if (!is_arg(key) && !is_arg(idx)) {
            int ix;
            const EvflValue *pk = fc.events[i].params.get("itemKey");
            if (key && key->type == EvflValue::String && str_is(pk, key->s) && int_of(idx, ix) && combos.count({key->s, ix})) {
                fc.events[i].params.set("messageEntry", EvflValue::of_str(ks_entry(key->s, ix)));
                n++;
            }
            continue;
        }
        if (!(is_arg(key) && is_arg(idx))) continue;
        // argument give (chests): itemKey == K and itemIndex == i -> a get event with the Ks entry
        const EvflValue *kc = fc.events[i].params.get("keepCarry");
        EvflValue keep = kc ? *kc : EvflValue::of_bool(false);
        std::string karg = key->s, iarg = idx->s, ev = fc.events[i].name, nxt_name = fc.events[nx].name;
        size_t before = fc.events.size();
        std::string head = ev;
        for (const std::string &k : keys) {
            std::string idx_head = ev;
            for (const auto &c : combos) {
                if (c.first != k) continue;
                std::string get = f.action("Link", "GenericItemGetSequenceByKey",
                                           {{"itemKey", k}, {"keepCarry", keep}, {"messageEntry", ks_entry(k, c.second)}}, nxt_name);
                idx_head = f.sw("FlowControl", "CompareInt", {{"value1", iarg}, {"value2", c.second}}, {{0, get}, {1, idx_head}});
            }
            head = f.sw("FlowControl", "CompareString", {{"value1", karg}, {"value2", k}}, {{0, idx_head}, {1, head}});
        }
        std::set<uint16_t> fresh;
        for (size_t j = before; j < fc.events.size(); j++) fresh.insert((uint16_t)j);
        retarget(fc, (uint16_t)i, event_index(fc, head), fresh);
        n++;
    }
    return n;
}

}  // namespace

void keysanity_text_apply(SeedBuild &b) {
    Combos combos = ks_used(b.decisions());
    if (combos.empty()) return;
    for (const std::string &name : written_flows(b)) {
        if (name == "Item.bfevfl") continue;
        EventFlow flow = b.read_flow(name);
        if (ks_patch_flow(b, flow, combos)) b.write_flow(name, flow);
    }
    ensure_item_flow(b);
    {
        EventFlow flow = b.read_flow("Item.bfevfl");
        Flow f(b, flow.flowchart);
        for (const auto &c : combos) item_entry(f, ks_entry(c.first, c.second));
        b.write_flow("Item.bfevfl", flow);
    }
    for (const std::string &rel : system_msbts(b)) {  // write_messages
        std::string lang = rel.substr(rel.find('/') + 1);
        lang = lang.substr(0, lang.find('/'));
        bool english = lang.size() >= 2 && !lang.compare(lang.size() - 2, 2, "en");
        Msbt sysm = need_msbt(b, rel);
        Msbt place = need_msbt(b, replace_all(rel, "System.msbt", "Place.msbt"));
        for (const auto &c : combos) {
            const char *msg = nullptr;
            for (const KsItem &x : KS_ITEMS)
                if (c.first == x.key) msg = x.msg;
            const std::string *get = sysm.text(msg), *name = place.text(place_label(c.second));
            if (!get || !name) continue;
            std::string g = *get, nm = *name;
            sysm.set(ks_entry(c.first, c.second), ks_compose(g, nm, english));
        }
        write_msbt(b, rel, sysm);
    }
}

// ======== bomb_bag_text.py
namespace {
const std::string BOMB_FLAG = "unused0431";

std::string bomb_bag_text(const std::string &wait) {
    return "You found your " + RED + "Bomb Bag" + WHITE + "! Now you can\ncarry bombs again." + wait + std::string(1, '\0');
}
std::string powder_bag_text(const std::string &wait) {
    return "You found your " + RED + "Powder Bag" + WHITE + "! Now you can\ncarry Magic Powder again." + wait +
           std::string(1, '\0');
}
std::string bomb_no_bag_text(const std::string &wait) {
    return "You got a " + RED + "Bomb" + WHITE + "! Too bad you have\nnowhere to put it.." + wait + std::string(1, '\0');
}
std::string powder_no_bag_text(const std::string &wait) {
    return "You got some " + RED + "Magic Powder" + WHITE + "! But you have\nnowhere to put it and it blew away.." + wait +
           std::string(1, '\0');
}

// bomb_bag_text.add_message: Item.bfevfl entry `entry` showing System:<entry>, the text in every language
void add_message(SeedBuild &b, const std::string &entry, std::string (*text_fn)(const std::string &)) {
    ensure_item_flow(b);
    EventFlow flow = b.read_flow("Item.bfevfl");
    Flow f(b, flow.flowchart);
    item_entry(f, entry);
    b.write_flow("Item.bfevfl", flow);
    for (const std::string &rel : system_msbts(b)) {
        Msbt m = need_msbt(b, rel);
        const std::string *gb = m.text("GetBomb");
        m.set(entry, text_fn(last_wait(gb ? *gb : "")));
        write_msbt(b, rel, m);
    }
}
}  // namespace

void bomb_bag_text_apply(SeedBuild &b) {
    if (!setting_or(b.decisions(), "shuffle-bombs", false)) return;
    int n = 0;
    for (const std::string &name : written_flows(b)) {
        EventFlow flow = b.read_flow(name);
        EvflFlowchart &fc = flow.flowchart;
        Flow f(b, fc);
        bool changed = false;
        size_t count = fc.events.size();
        for (size_t i = 0; i < count; i++) {
            EvflContainer *prm;
            std::string act;
            if (!action_of(fc, (uint16_t)i, nullptr, &act, &prm)) continue;
            if (act != "GenericItemGetSequenceByKey" || !str_is(prm->get("itemKey"), "Bomb") || truthy(prm->get("messageEntry")))
                continue;
            prm->set("messageEntry", EvflValue::of_str("BombBag"));
            prm->set("itemKey", EvflValue::of_str("MagicPowder"));
            std::string off = f.action("EventFlags", "SetFlag", {{"symbol", BOMB_FLAG}, {"value", false}},
                                       name_of(fc, fc.events[i].next));
            std::string on = f.action("EventFlags", "SetFlag", {{"symbol", BOMB_FLAG}, {"value", true}}, fc.events[i].name);
            uint16_t on_ev = event_index(fc, on), off_ev = event_index(fc, off);
            retarget(fc, (uint16_t)i, on_ev, {on_ev});
            fc.events[i].next = off_ev;
            changed = true;
            n++;
        }
        if (changed) b.write_flow(name, flow);
    }
    if (n) add_message(b, "BombBag", bomb_bag_text);
}

void powder_bag_text_apply(SeedBuild &b) {
    if (!setting_or(b.decisions(), "shuffle-powder", false)) return;
    int n = 0;
    for (const std::string &name : written_flows(b)) {
        EventFlow flow = b.read_flow(name);
        EvflFlowchart &fc = flow.flowchart;
        bool changed = false;
        for (size_t i = 0; i < fc.events.size(); i++) {
            EvflContainer *prm;
            std::string act;
            if (!action_of(fc, (uint16_t)i, nullptr, &act, &prm)) continue;
            if (act == "GenericItemGetSequenceByKey" && str_is(prm->get("itemKey"), "MagicPowder") &&
                !truthy(prm->get("messageEntry"))) {
                prm->set("messageEntry", EvflValue::of_str("PowderBag"));
                changed = true;
                n++;
            }
        }
        if (changed) b.write_flow(name, flow);
    }
    if (n) add_message(b, "PowderBag", powder_bag_text);
}

// ======== refill_gets.py
void refill_gets_apply(SeedBuild &b) {
    const int REFILL_INDEX = 80;
    static const std::map<std::string, std::string> FOUND = {{"Bomb", "unused0424"}, {"MagicPowder", "GetMagicPowder"}, {"Arrow", ""}};
    static const std::map<std::string, std::string> NO_BAG = {{"Bomb", "BombNoBag"}, {"MagicPowder", "PowderNoBag"}};
    std::set<std::string> found;
    if (setting_or(b.decisions(), "shuffle-bombs", false)) found.insert("Bomb");
    if (setting_or(b.decisions(), "shuffle-powder", false)) found.insert("MagicPowder");
    std::set<std::string> no_bag;
    for (const std::string &name : written_flows(b)) {
        EventFlow flow = b.read_flow(name);
        EvflFlowchart &fc = flow.flowchart;
        Flow f(b, fc);
        size_t count = fc.events.size();  // (the events as they were: list(fc.events))
        bool shared = false;
        for (size_t i = 0; i < count; i++) {
            EvflContainer *prm;
            if (act_name(fc, (uint16_t)i) != "AddItemByKey") continue;
            action_of(fc, (uint16_t)i, nullptr, nullptr, &prm);
            const EvflValue *ix = prm->get("index");
            shared |= is_arg(ix) && ix->s == "itemIndex";
        }
        bool changed = false;
        for (size_t a = 0; a < count; a++) {
            EvflContainer *prm;
            std::string act;
            if (!action_of(fc, (uint16_t)a, nullptr, &act, &prm) || act != "AddItemByKey") continue;
            const EvflValue *kv = prm->get("itemKey");
            if (!is_str(kv) || !FOUND.count(kv->s)) continue;
            std::string item = kv->s;
            const EvflValue *iv = prm->get("index");
            int index = 0;
            bool is_int = !is_arg(iv) && int_of(iv, index);
            bool refill_none;  // (refill is None: always a refill)
            if (is_int && index == REFILL_INDEX) {
                refill_none = true;
            } else if (is_int && index == -1 && shared) {
                prm->set("index", EvflValue::of_arg("itemIndex"));
                refill_none = false;
            } else {
                continue;
            }
            changed = true;
            // the found flag: skipped for a refill
            const std::string &fl = FOUND.at(item);
            int flag = -1;
            for (size_t e = 0; e < count && !fl.empty(); e++) {
                EvflContainer *fp;
                std::string fa;
                if (!action_of(fc, (uint16_t)e, nullptr, &fa, &fp) || fa != "SetFlag") continue;
                if (str_is(fp->get("symbol"), fl) && fc.events[e].next == a) { flag = (int)e; break; }
            }
            if (flag >= 0) {
                if (refill_none) {
                    retarget(fc, (uint16_t)flag, (uint16_t)a, {});
                } else {
                    std::string s = f.sw("FlowControl", "CompareInt", {{"value1", "itemIndex"}, {"value2", REFILL_INDEX}},
                                         {{0, fc.events[a].name}, {1, fc.events[flag].name}});
                    uint16_t si = event_index(fc, s);
                    retarget(fc, (uint16_t)flag, si, {si});
                }
            }
            // found bombs / powder: a refill's own message (the bag's or not)
            uint16_t get = fc.events[a].next;
            EvflContainer *gp = nullptr;
            std::string gact;
            bool gok = action_of(fc, get, nullptr, &gact, &gp);
            if (!(found.count(item) && gok && gact == "GenericItemGetSequenceByKey" && str_is(gp->get("itemKey"), item)))
                continue;
            std::string after = name_of(fc, fc.events[get].next), have;
            if (refill_none) {
                gp->set("messageEntry", EvflValue::of_str(item));
                have = fc.events[get].name;
            } else {
                have = f.action("Link", "GenericItemGetSequenceByKey",
                                {{"itemKey", item}, {"keepCarry", false}, {"messageEntry", item}}, after);
            }
            std::string none = f.action("Link", "GenericItemGetSequenceByKey",
                                        {{"itemKey", item}, {"keepCarry", false}, {"messageEntry", NO_BAG.at(item)}}, after);
            std::string msg = f.sw("EventFlags", "CheckFlag", {{"symbol", fl}}, {{0, none}, {1, have}});
            if (refill_none) {
                fc.events[a].next = event_index(fc, msg);
            } else {
                std::string s = f.sw("FlowControl", "CompareInt", {{"value1", "itemIndex"}, {"value2", REFILL_INDEX}},
                                     {{0, msg}, {1, fc.events[get].name}});
                fc.events[a].next = event_index(fc, s);
            }
            no_bag.insert(item);
        }
        if (changed) b.write_flow(name, flow);
    }
    for (const std::string &item : no_bag)  // (sorted: Bomb, MagicPowder)
        add_message(b, NO_BAG.at(item), item == "Bomb" ? bomb_no_bag_text : powder_no_bag_text);
}

// ======== silent_pickups.py
namespace {
const int SILENT_BASE = 3000;
bool sp_wanted(const EvflValue *key) {
    if (!is_str(key)) return false;
    const std::string &k = key->s;
    return k == "HeartPiece" || k == "Seashell" || k == "HeartContainer" || k == "Heart" || k == "Apple" ||
           k == "GreenApple" || !k.compare(0, 5, "Rupee");
}
std::string sp_se(const std::string &k) {
    if (!k.compare(0, 5, "Rupee")) return "SE_SY_GET_RUPY";
    if (k == "HeartPiece" || k == "Seashell") return "SE_SY_GET_ITEM";
    if (k == "HeartContainer") return "SE_SY_HEART_UP";
    if (k == "Heart" || k == "Apple" || k == "GreenApple") return "SE_SY_HEART_RECOVERY";
    throw std::runtime_error("silent pickups: no sound for " + k);  // (KeyError)
}

int sp_patch(SeedBuild &b, EvflFlowchart &fc) {
    Flow f(b, fc);
    int n = 0;
    std::set<uint16_t> done;
    for (size_t p = 0; p < fc.entry_points.size(); p++) {
        std::vector<uint16_t> path;
        uint16_t e = fc.entry_points[p].main_event, add = EVFL_NONE;
        while (e != EVFL_NONE && path.size() < 16) {
            EvflContainer *prm;
            std::string act;
            if (!action_of(fc, e, nullptr, &act, &prm)) break;
            path.push_back(e);
            if (act == "AddItemByKey" && sp_wanted(prm->get("itemKey"))) { add = e; break; }
            e = fc.events[e].next;
        }
        if (add == EVFL_NONE || done.count(add)) continue;
        done.insert(add);
        std::string key = fc.events[add].params.get("itemKey")->s;
        for (uint16_t g : path) {
            EvflContainer *gp;
            std::string ga;
            action_of(fc, g, nullptr, &ga, &gp);
            if (ga == "GenericItemGetSequenceByKey" && str_is(gp->get("itemKey"), key)) retarget(fc, g, fc.events[g].next, {});
        }
        EvflContainer &prm = fc.events[add].params;
        const EvflValue *iv = prm.get("index");
        int idx = -1;
        bool ok = !iv || int_of(iv, idx);  // (prm.get('index', -1): an int)
        if ((key == "HeartPiece" || key == "HeartContainer" || key == "Seashell") && ok && -1 <= idx && idx < 1000)
            prm.set("index", EvflValue::of_int(SILENT_BASE + 1 + idx));
        // the pickup sound right before the add
        std::string se = f.action("Audio", "PlayOneshotSystemSE", {{"label", sp_se(key)}, {"volume", 1.0}, {"pitch", 1.0}},
                                  fc.events[add].name);
        uint16_t si = event_index(fc, se);
        retarget(fc, add, si, {});
        fc.events[si].next = add;
        n++;
    }
    return n;
}

int sp_patch_item_common(SeedBuild &b, EvflFlowchart &fc) {
    Flow f(b, fc);
    int n = 0;
    for (size_t p = 0; p < fc.entry_points.size(); p++) {
        uint16_t first = fc.entry_points[p].main_event;
        if (act_name(fc, first) != "Deactivate") continue;
        std::vector<uint16_t> chain;
        uint16_t e = fc.events[first].next;
        while (e != EVFL_NONE && action_of(fc, e, nullptr, nullptr, nullptr) && chain.size() < 8) {
            chain.push_back(e);
            e = fc.events[e].next;
        }
        uint16_t add = EVFL_NONE;
        bool get = false;
        for (uint16_t x : chain) {
            std::string a = act_name(fc, x);
            if (add == EVFL_NONE && a == "AddItemByKey") add = x;
            get |= a == "GenericItemGetSequenceByKey";
        }
        if (add == EVFL_NONE || !get) continue;
        EvflContainer prm = fc.events[add].params;
        std::string after = name_of(fc, fc.events[add].next);
        std::string off = f.action("EventFlags", "SetFlag", {{"symbol", "unused0432"}, {"value", false}}, after);
        std::string give = f.action("Inventory", "AddItemByKey", {}, off);
        f.event(give).params = prm;
        std::string on = f.action("EventFlags", "SetFlag", {{"symbol", "unused0432"}, {"value", true}}, give);
        on = f.action("Audio", "PlayOneshotSystemSE", {{"label", "SE_SY_GET_ITEM"}, {"volume", 1.0}, {"pitch", 1.0}}, on);
        std::string sw = f.sw("FlowControl", "CompareString", {{"value1", "itemKey"}, {"value2", "Seashell"}},
                              {{0, on}, {1, fc.events[chain[0]].name}});
        fc.events[first].next = event_index(fc, sw);
        n++;
    }
    return n;
}
}  // namespace

void silent_pickups_apply(SeedBuild &b) {
    for (const char *name : {"SinkingSword.bfevfl", "ItemCommon.bfevfl"}) {
        EventFlow flow = b.read_flow(name);
        if (std::string(name) == "SinkingSword.bfevfl") sp_patch(b, flow.flowchart);
        else sp_patch_item_common(b, flow.flowchart);
        b.write_flow(name, flow);
    }
}

}  // namespace ours
}  // namespace rando
