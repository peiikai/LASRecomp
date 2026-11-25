// LAS Recomp's item steps that only write the seed's tables: quick pickups (tools/rando/quick_pickups.py apply) and
// flags.tsv (seed_tables.py write_flags), ported (ours_items.cpp). Both read the seed's files as the generator left
// them (the seed's copy, else the game's) and write nothing into the game files.
#include <stdio.h>
#include <string.h>

#include <map>
#include <set>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "ours_items_lib.h"

namespace rando {
namespace ours {
namespace {

using las::at;

// the Python value's truth (bool(x)) of an event parameter
bool truthy(const EvflValue &v) {
    switch (v.type) {
    case EvflValue::Bool: return v.b;
    case EvflValue::Int: return v.i != 0;
    case EvflValue::Float: return v.f != 0;
    case EvflValue::String: return !v.s.empty();
    default: return true;
    }
}

// int(x) of a number parameter
int64_t int_of(const EvflValue &v) {
    switch (v.type) {
    case EvflValue::Bool: return v.b;
    case EvflValue::Int: return v.i;
    case EvflValue::Float: return (int64_t)v.f;  // (int(): towards zero)
    default: throw std::runtime_error("quick pickups: not a number");  // (int() of a str or list: an error there)
    }
}

struct Quick {
    bool has_flag = false, has_se = false;
    std::string flag, se;
    const EvflContainer *add = nullptr;
};

// quick(): the entry's events, followed from its main event, are only the quick steps (each once) with an add and a
// Destroy; false otherwise
bool quick(const EvflFlowchart &fc, const EvflEntryPoint &ep, Quick &q) {
    std::set<std::pair<std::string, std::string>> seen;
    bool destroy = false;
    for (uint16_t i = ep.main_event; i != EVFL_NONE;) {
        const EvflEvent &e = fc.events.at(i);
        if (e.type != EvflEvent::Action) return false;
        const EvflActor &actor = fc.actors.at(e.actor);
        std::pair<std::string, std::string> a{actor.id.name, actor.actions.at(e.action)};
        bool step = (a.first == "EventFlags" && a.second == "SetFlag") ||
                    (a.first == "Audio" && a.second == "PlayOneshotSystemSE") ||
                    (a.first == "Inventory" && a.second == "AddItemByKey") || (a.first == "SinkingSword" && a.second == "Destroy");
        if (!step || seen.count(a)) return false;
        seen.insert(a);
        const EvflContainer &p = e.params;
        if (a.second == "SetFlag") {
            const EvflValue *v = p.get("value");
            if (!v || v->type != EvflValue::Bool || !v->b) return false;  // (`is not True`)
            const EvflValue *s = p.get("symbol");
            q.has_flag = s != nullptr;
            q.flag = s ? s->s : "";
        } else if (a.second == "PlayOneshotSystemSE") {
            const EvflValue *s = p.get("label");
            q.has_se = s != nullptr;
            q.se = s ? s->s : "";
        } else if (a.second == "AddItemByKey") {
            const EvflValue *eq = p.get("autoEquip"), *k = p.get("itemKey");
            if ((eq && truthy(*eq)) || !k || k->type != EvflValue::String) return false;
            q.add = &p;
        } else {
            destroy = true;
        }
        i = e.next;
    }
    return q.add && destroy;
}

}  // namespace

// quick_pickups.tsv: the SinkingSword.bfevfl entries the runtime gives without an event (entry, flag index or -1,
// itemID, count, index, sound or '-')
void quick_pickups_apply(SeedBuild &b) {
    EventFlow flow = b.read_flow("SinkingSword.bfevfl");
    std::map<std::string, int> flags;  // (a later row of a name wins, as the Python's dict)
    for (const GStruct &v : b.read_sheet("GlobalFlags.gsheet").values) flags[at(v, "symbol").s] = at(v, "index").i;
    std::map<std::string, int> items;  // (the first row of a symbol wins: setdefault)
    for (const GStruct &v : b.read_sheet("Items.gsheet").values) items.insert({at(v, "symbol").s, at(v, "itemID").i});
    const EvflFlowchart &fc = flow.flowchart;
    std::string out;
    for (const EvflEntryPoint &ep : fc.entry_points) {
        Quick q;
        if (!quick(fc, ep, q)) continue;
        const std::string &key = q.add->get("itemKey")->s;
        if (!items.count(key) || (q.has_flag && !flags.count(q.flag))) continue;
        const EvflValue *count = q.add->get("count"), *index = q.add->get("index");
        char buf[64];
        snprintf(buf, sizeof buf, "\t%d\t%d\t%lld\t%lld\t", q.flag.empty() ? -1 : flags.at(q.flag), items.at(key),
                 (long long)(count ? int_of(*count) : 1), (long long)(index ? int_of(*index) : -1));
        out += ep.name + buf + (q.se.empty() ? "-" : q.se) + "\n";
    }
    b.table_write("quick_pickups.tsv", out);
}

// flags.tsv: GlobalFlags name -> index for the flags the runtime reads (the trade items' flags, pets.c's), and the
// Bananas' "traded" alias: KikiGone with open-bridge (LAS sets StickDrop at the start there), else StickDrop
void seed_tables_write_flags(SeedBuild &b) {
    static const std::set<std::string> WANT_EXACT = {
        "RibbonDrop", "DogfoodDrop", "BananaDrop", "HoneycombDrop", "PineappleGet", "HibiscusDrop", "LetterDrop",
        "BroomDrop", "FishhookDrop", "BrassiereDrop", "ScaleDrop", "MarthaStatueOpened", "unused0427", "unused0428",
        "unused0429", "unused0430", "GhostClear1", "Ghost2_Clear", "Ghost3_Clear", "Ghost4_Clear", "WalrusAwaked",
        "EighthClear"};
    GSheet sheet = b.read_sheet("GlobalFlags.gsheet");
    std::string out;
    std::map<std::string, int> idx;
    for (const GStruct &v : sheet.values) {
        const std::string &sym = at(v, "symbol").s;
        int i = at(v, "index").i;
        idx[sym] = i;
        if (!sym.compare(0, 5, "Trade") || WANT_EXACT.count(sym)) out += sym + "\t" + std::to_string(i) + "\n";
    }
    // (settings.json's open-bridge: the decisions' settings; a missing or null one gives no alias)
    yyjson_val *bridge = las::setting_val(b.decisions(), "open-bridge");
    if (bridge && !yyjson_is_null(bridge)) {
        auto it = idx.find(las::py_truthy(bridge) ? "KikiGone" : "StickDrop");
        if (it != idx.end()) out += "TradeBananasDone\t" + std::to_string(it->second) + "\n";
    }
    b.table_write("flags.tsv", out);
}

}  // namespace ours
}  // namespace rando
